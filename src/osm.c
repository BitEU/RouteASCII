#include "osm.h"
#include "mbtiles.h"
#include "mvt.h"
#include "geo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * MBTiles-backed OSM cache.
 *
 * We open a single MBTiles file (data/us.mbtiles) at startup. On each
 * viewport update we compute the set of z=14 tiles that cover the view,
 * look them up in an in-memory LRU, and decode any misses from the
 * MBTiles file via MVT. cache->layers is rebuilt by concatenating
 * features from every tile in the visible set.
 *
 * The LRU caps how many decoded tiles we keep in memory at once. A
 * typical zoom-10 viewport covers ~16 tiles; a wide zoom-8 viewport can
 * cover ~200. We cap at 512 tiles, and when a tile falls out of the
 * visible set it stays in the LRU until evicted.
 * -------------------------------------------------------------------------*/

#define OSM_MIN_TILE_ZOOM  6   /* below this, Natural Earth is enough */
#define OSM_TILE_Z        14   /* MBTiles are baked at z=14 */
#define LRU_CAP          512
#define MBTILES_DEFAULT_PATH "data/us.mbtiles"

/* Resolve the MBTiles path: ROUTEASCII_MBTILES env var wins if set, else
 * fall back to data/us.mbtiles. Set the env var when testing with a
 * smaller extract (e.g. data/nj.mbtiles). */
static const char *resolve_mbtiles_path(void)
{
    const char *env = getenv("ROUTEASCII_MBTILES");
    if (env && *env) return env;
    return MBTILES_DEFAULT_PATH;
}

typedef struct TileEntry {
    int       z, x, y;
    int       valid;
    /* Per-tile decoded layers — same shape as OsmCache::layers, but
     * holding only this tile's features. */
    GeoLayer  layers[OSM_LAYER_COUNT];
    /* LRU bookkeeping: usage counter incremented on touch; oldest
     * entry wins eviction. Simpler than a real doubly-linked list. */
    unsigned  last_used;
} TileEntry;

static Mbtiles  *g_mb = NULL;
static TileEntry g_lru[LRU_CAP];
static int       g_lru_n = 0;
static unsigned  g_lru_clock = 0;

/* Track the set of currently visible tiles so we can skip work when the
 * viewport hasn't moved into new tiles. */
static int g_vis_x0 = 0, g_vis_y0 = 0, g_vis_x1 = -1, g_vis_y1 = -1;
static int g_vis_z  = -1;

/* ---------------------------------------------------------------------------
 * LRU helpers.
 * ---------------------------------------------------------------------*/

static TileEntry *lru_find(int z, int x, int y)
{
    for (int i = 0; i < g_lru_n; i++) {
        if (g_lru[i].valid &&
            g_lru[i].z == z && g_lru[i].x == x && g_lru[i].y == y)
            return &g_lru[i];
    }
    return NULL;
}

static void tile_entry_free_layers(TileEntry *te)
{
    for (int i = 0; i < OSM_LAYER_COUNT; i++) {
        geolayer_free(&te->layers[i]);
    }
}

static TileEntry *lru_allocate(int z, int x, int y)
{
    /* First try a free slot. */
    if (g_lru_n < LRU_CAP) {
        TileEntry *te = &g_lru[g_lru_n++];
        memset(te, 0, sizeof(*te));
        te->z = z; te->x = x; te->y = y; te->valid = 1;
        te->last_used = ++g_lru_clock;
        return te;
    }
    /* Otherwise evict the least-recently-used entry. */
    int victim = 0;
    for (int i = 1; i < g_lru_n; i++) {
        if (g_lru[i].last_used < g_lru[victim].last_used) victim = i;
    }
    TileEntry *te = &g_lru[victim];
    tile_entry_free_layers(te);
    memset(te, 0, sizeof(*te));
    te->z = z; te->x = x; te->y = y; te->valid = 1;
    te->last_used = ++g_lru_clock;
    return te;
}

static void lru_touch(TileEntry *te)
{
    te->last_used = ++g_lru_clock;
}

/* ---------------------------------------------------------------------------
 * Tile load: pull from MBTiles, decode, populate LRU entry.
 * ---------------------------------------------------------------------*/

static TileEntry *load_tile(int z, int x, int y)
{
    TileEntry *te = lru_find(z, x, y);
    if (te) { lru_touch(te); return te; }
    if (!g_mb) return NULL;

    unsigned char *blob = NULL;
    size_t blob_size = 0;
    if (mbtiles_get_tile(g_mb, z, x, y, &blob, &blob_size) != 0) {
        /* Cache the miss as an empty slot so we don't try again. */
        te = lru_allocate(z, x, y);
        return te;
    }

    te = lru_allocate(z, x, y);
    /* The MVT decoder appends into an OsmCache's layer array. We want
     * it to append into the tile entry's per-tile layer array instead,
     * so build a temporary OsmCache "view" that points at te->layers,
     * run the decode, then copy the (updated) layer structs back. Each
     * GeoLayer contains only a features pointer + counts, so the copy
     * is safe — we're transferring ownership of the heap features
     * buffer from shim to te. */
    OsmCache shim;
    memset(&shim, 0, sizeof(shim));
    mvt_decode_tile(&shim, z, x, y, blob, blob_size);
    memcpy(te->layers, shim.layers, sizeof(shim.layers));
    free(blob);
    return te;
}

/* ---------------------------------------------------------------------------
 * Merge one tile's layers into the public cache->layers. We can't move
 * the features (the LRU still owns them), so we deep-copy them. This is
 * a few hundred malloc's per tile worst case — fast enough that we
 * redo it any time the visible set changes.
 * ---------------------------------------------------------------------*/

static GeoRing ring_dup(const GeoRing *src)
{
    GeoRing dst = {0};
    if (src->npoints <= 0 || !src->coords) return dst;
    dst.coords = malloc((size_t)src->npoints * 2 * sizeof(float));
    if (!dst.coords) return dst;
    memcpy(dst.coords, src->coords, (size_t)src->npoints * 2 * sizeof(float));
    dst.npoints = src->npoints;
    return dst;
}

static void layer_append_copy(GeoLayer *dst, const GeoLayer *src)
{
    for (int i = 0; i < src->nfeatures; i++) {
        const GeoFeature *sf = &src->features[i];
        GeoFeature nf = *sf;
        nf.rings = calloc((size_t)sf->nrings, sizeof(GeoRing));
        if (!nf.rings) continue;
        nf.nrings = 0;
        for (int r = 0; r < sf->nrings; r++) {
            nf.rings[nf.nrings] = ring_dup(&sf->rings[r]);
            if (nf.rings[nf.nrings].coords) nf.nrings++;
        }
        if (nf.nrings > 0) {
            geolayer_add(dst, nf);
        } else {
            free(nf.rings);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ---------------------------------------------------------------------*/

void osm_init(OsmCache *cache)
{
    memset(cache, 0, sizeof(*cache));
    for (int i = 0; i < OSM_LAYER_COUNT; i++) geolayer_init(&cache->layers[i]);

    if (g_mb) { mbtiles_close(g_mb); g_mb = NULL; }
    for (int i = 0; i < g_lru_n; i++) tile_entry_free_layers(&g_lru[i]);
    memset(g_lru, 0, sizeof(g_lru));
    g_lru_n = 0;
    g_lru_clock = 0;
    g_vis_z = -1;

    const char *path = resolve_mbtiles_path();
    g_mb = mbtiles_open(path);
    if (!g_mb) {
        cache->mbtiles_ok = 0;
        snprintf(cache->status, sizeof(cache->status),
                 "No %s - zoom 6+ will be bare. Run tilemaker to generate.",
                 path);
        fprintf(stderr, "[osm] MBTiles file not found at %s; OSM detail disabled\n",
                path);
    } else {
        cache->mbtiles_ok = 1;
        snprintf(cache->status, sizeof(cache->status),
                 "MBTiles loaded (z=%d..%d)",
                 mbtiles_minzoom(g_mb), mbtiles_maxzoom(g_mb));
    }
}

void osm_free(OsmCache *cache)
{
    for (int i = 0; i < OSM_LAYER_COUNT; i++) geolayer_free(&cache->layers[i]);
    memset(cache, 0, sizeof(*cache));

    for (int i = 0; i < g_lru_n; i++) tile_entry_free_layers(&g_lru[i]);
    memset(g_lru, 0, sizeof(g_lru));
    g_lru_n = 0;

    if (g_mb) { mbtiles_close(g_mb); g_mb = NULL; }
}

/* Convert viewport to tile range at the given tile zoom. We use geo.c's
 * slippy-tile math directly — the map projection already matches. */
static void viewport_tile_range(MapView *mv, int tile_z,
                                int *x0, int *y0, int *x1, int *y1)
{
    double lat_tl, lon_tl, lat_br, lon_br;
    map_screen_to_latlon(mv, 0, 0, &lat_tl, &lon_tl);
    map_screen_to_latlon(mv, mv->screen_w - 1, mv->screen_h - 1, &lat_br, &lon_br);

    /* Clamp lat to the Mercator domain. */
    if (lat_tl >  85.0) lat_tl =  85.0;
    if (lat_tl < -85.0) lat_tl = -85.0;
    if (lat_br >  85.0) lat_br =  85.0;
    if (lat_br < -85.0) lat_br = -85.0;

    int ax, ay, bx, by;
    geo_latlon_to_tile(lat_tl, lon_tl, tile_z, &ax, &ay);
    geo_latlon_to_tile(lat_br, lon_br, tile_z, &bx, &by);

    *x0 = ax < bx ? ax : bx;
    *x1 = ax > bx ? ax : bx;
    *y0 = ay < by ? ay : by;
    *y1 = ay > by ? ay : by;
}

/* Pick the tile zoom we should pull from the mbtiles for a given map
 * zoom. Rule: match the map zoom, clamped to what the mbtiles actually
 * contains. This keeps the on-screen feature density roughly constant:
 * at map zoom 9 we pull z=9 tiles (each covering the same slippy-tile
 * area the viewport shows), and at map zoom 14+ we max out at z=14
 * since that's our basezoom. */
static int pick_tile_zoom(const MapView *mv)
{
    int z = mv->zoom;
    int zmin = mbtiles_minzoom(g_mb);
    int zmax = mbtiles_maxzoom(g_mb);
    if (z < zmin) z = zmin;
    if (z > zmax) z = zmax;
    return z;
}

void osm_request_viewport(OsmCache *cache, MapView *mv)
{
    if (!g_mb) return;                        /* file missing — nothing to do */
    if (mv->zoom < OSM_MIN_TILE_ZOOM) {
        /* Too far out for detailed tiles. Clear any stale features. */
        if (g_vis_z != -1) {
            for (int i = 0; i < OSM_LAYER_COUNT; i++) geolayer_free(&cache->layers[i]);
            g_vis_z = -1;
        }
        return;
    }

    int tile_z = pick_tile_zoom(mv);

    int x0, y0, x1, y1;
    viewport_tile_range(mv, tile_z, &x0, &y0, &x1, &y1);

    /* Hard safety cap. With dynamic tile_z this should almost never
     * trigger — a full-screen view at any single zoom level covers at
     * most a few hundred tiles at that zoom. The cap is here as a
     * last-resort guard against runaway memory. */
    int tiles_wide = x1 - x0 + 1;
    int tiles_tall = y1 - y0 + 1;
    long ntiles = (long)tiles_wide * (long)tiles_tall;
    if (ntiles > 2048) {
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        int half = 22;
        x0 = cx - half; x1 = cx + half;
        y0 = cy - half; y1 = cy + half;
        tiles_wide = x1 - x0 + 1;
        tiles_tall = y1 - y0 + 1;
        ntiles = (long)tiles_wide * (long)tiles_tall;
    }

    /* Skip rebuild if the visible set hasn't changed. */
    if (g_vis_z == tile_z &&
        g_vis_x0 == x0 && g_vis_x1 == x1 &&
        g_vis_y0 == y0 && g_vis_y1 == y1) {
        return;
    }
    g_vis_z  = tile_z;
    g_vis_x0 = x0; g_vis_x1 = x1;
    g_vis_y0 = y0; g_vis_y1 = y1;

    /* Rebuild cache->layers from the visible tile set. */
    for (int i = 0; i < OSM_LAYER_COUNT; i++) geolayer_free(&cache->layers[i]);

    int loaded = 0, missed = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            TileEntry *te = load_tile(tile_z, x, y);
            if (!te) { missed++; continue; }
            loaded++;
            for (int l = 0; l < OSM_LAYER_COUNT; l++) {
                layer_append_copy(&cache->layers[l], &te->layers[l]);
            }
        }
    }

    snprintf(cache->status, sizeof(cache->status),
             "OSM z%d: %d tiles (%ldx%ld) %s",
             tile_z, loaded, (long)tiles_wide, (long)tiles_tall,
             missed ? "(some missing)" : "");
}

int osm_tick(OsmCache *cache) { (void)cache; return 0; }
int osm_is_busy(OsmCache *cache) { (void)cache; return 0; }
