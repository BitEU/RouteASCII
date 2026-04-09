#include "osm.h"
#include "http.h"
#include "geo.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#define OSM_TILE_Z     13
#define OSM_MAX_TILES  512      /* soft cap on how many tiles we track */
#define OVERPASS_URL   "https://overpass-api.de/api/interpreter"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * Tile tracking.
 * -------------------------------------------------------------------------*/

typedef enum {
    TS_EMPTY = 0,
    TS_PENDING,
    TS_LOADED,
    TS_FAILED
} TileStatus;

typedef struct {
    int        x, y;
    TileStatus status;
    /* Per-tile feature indices into the cache's flat layers. We don't
     * actually track these — on cache invalidation we rebuild the whole
     * thing. Kept simple on purpose. */
} TileSlot;

static TileSlot g_tiles[OSM_MAX_TILES];
static int      g_tile_count = 0;

/* Find a tile slot by coords, or -1. */
static int tile_find(int x, int y)
{
    for (int i = 0; i < g_tile_count; i++) {
        if (g_tiles[i].x == x && g_tiles[i].y == y) return i;
    }
    return -1;
}

static int tile_add(int x, int y, TileStatus st)
{
    if (g_tile_count >= OSM_MAX_TILES) {
        /* Evict the oldest loaded tile — LRU-ish. */
        for (int i = 0; i < g_tile_count; i++) {
            if (g_tiles[i].status == TS_LOADED) {
                g_tiles[i].x = x; g_tiles[i].y = y; g_tiles[i].status = st;
                return i;
            }
        }
        return -1;
    }
    g_tiles[g_tile_count].x = x;
    g_tiles[g_tile_count].y = y;
    g_tiles[g_tile_count].status = st;
    return g_tile_count++;
}

/* ---------------------------------------------------------------------------
 * Disk cache paths.
 * -------------------------------------------------------------------------*/

static void mkdirp(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return;
    MKDIR(path);
}

static void tile_path(int x, int y, char *out, size_t out_sz)
{
    mkdirp("data");
    mkdirp("data/osm_cache");
    char dir[256];
    snprintf(dir, sizeof(dir), "data/osm_cache/%d", OSM_TILE_Z);
    mkdirp(dir);
    snprintf(dir, sizeof(dir), "data/osm_cache/%d/%d", OSM_TILE_Z, x);
    mkdirp(dir);
    snprintf(out, out_sz, "data/osm_cache/%d/%d/%d.json", OSM_TILE_Z, x, y);
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

/* ---------------------------------------------------------------------------
 * Tile <-> bbox conversion.
 * -------------------------------------------------------------------------*/

static void tile_bbox(int x, int y, double *s, double *w, double *n, double *e)
{
    int N = 1 << OSM_TILE_Z;
    *w = (double)x / N * 360.0 - 180.0;
    *e = (double)(x + 1) / N * 360.0 - 180.0;
    double n_rad = atan(sinh(M_PI * (1.0 - 2.0 * (double)y / N)));
    double s_rad = atan(sinh(M_PI * (1.0 - 2.0 * (double)(y + 1) / N)));
    *n = n_rad * 180.0 / M_PI;
    *s = s_rad * 180.0 / M_PI;
}

static void latlon_to_tile(double lat, double lon, int *x, int *y)
{
    int N = 1 << OSM_TILE_Z;
    double lat_rad = lat * M_PI / 180.0;
    *x = (int)floor((lon + 180.0) / 360.0 * N);
    *y = (int)floor((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * N);
    if (*x < 0) *x = 0; if (*x >= N) *x = N - 1;
    if (*y < 0) *y = 0; if (*y >= N) *y = N - 1;
}

/* ---------------------------------------------------------------------------
 * Layer storage helpers — wrap the GeoLayer/GeoFeature machinery for
 * linestring features built from Overpass way geometries.
 * -------------------------------------------------------------------------*/

static void push_way(GeoLayer *layer, cJSON *geom_arr)
{
    if (!cJSON_IsArray(geom_arr)) return;
    int n = cJSON_GetArraySize(geom_arr);
    if (n < 2) return;

    GeoFeature f = {0};
    f.type = GEOM_LINESTRING;
    f.bbox.min_lat =  90.0f; f.bbox.max_lat = -90.0f;
    f.bbox.min_lon = 180.0f; f.bbox.max_lon = -180.0f;
    f.rings = calloc(1, sizeof(GeoRing));
    if (!f.rings) return;
    f.rings[0].coords = malloc((size_t)n * 2 * sizeof(float));
    if (!f.rings[0].coords) { free(f.rings); return; }

    int ci = 0;
    for (int i = 0; i < n; i++) {
        cJSON *pt = cJSON_GetArrayItem(geom_arr, i);
        if (!cJSON_IsObject(pt)) continue;
        cJSON *jlat = cJSON_GetObjectItem(pt, "lat");
        cJSON *jlon = cJSON_GetObjectItem(pt, "lon");
        if (!cJSON_IsNumber(jlat) || !cJSON_IsNumber(jlon)) continue;
        float lat = (float)jlat->valuedouble;
        float lon = (float)jlon->valuedouble;
        f.rings[0].coords[ci * 2 + 0] = lon;
        f.rings[0].coords[ci * 2 + 1] = lat;
        if (lat < f.bbox.min_lat) f.bbox.min_lat = lat;
        if (lat > f.bbox.max_lat) f.bbox.max_lat = lat;
        if (lon < f.bbox.min_lon) f.bbox.min_lon = lon;
        if (lon > f.bbox.max_lon) f.bbox.max_lon = lon;
        ci++;
    }
    if (ci < 2) { free(f.rings[0].coords); free(f.rings); return; }
    f.rings[0].npoints = ci;
    f.nrings = 1;
    geolayer_add(layer, f);
}

/* Classify an OSM way by its tags into one of our layers. Returns -1 to
 * drop the way. */
static int classify_way(cJSON *tags)
{
    if (!cJSON_IsObject(tags)) return -1;
    cJSON *hw = cJSON_GetObjectItem(tags, "highway");
    if (cJSON_IsString(hw)) {
        const char *v = hw->valuestring;
        if (!strcmp(v, "motorway")    || !strcmp(v, "motorway_link") ||
            !strcmp(v, "trunk")       || !strcmp(v, "trunk_link") ||
            !strcmp(v, "primary")     || !strcmp(v, "primary_link"))
            return OSM_LAYER_ROAD_MAJOR;
        /* Drop footpaths/cycleways — too much noise. */
        if (!strcmp(v, "footway")   || !strcmp(v, "path") ||
            !strcmp(v, "cycleway")  || !strcmp(v, "steps") ||
            !strcmp(v, "track")     || !strcmp(v, "service"))
            return -1;
        return OSM_LAYER_ROAD_MINOR;
    }
    cJSON *wa = cJSON_GetObjectItem(tags, "waterway");
    if (cJSON_IsString(wa)) return OSM_LAYER_WATERWAY;
    cJSON *nat = cJSON_GetObjectItem(tags, "natural");
    if (cJSON_IsString(nat) && !strcmp(nat->valuestring, "water"))
        return OSM_LAYER_WATER;
    return -1;
}

/* Parse a full Overpass JSON response and append features to the cache. */
static int parse_tile_json(OsmCache *cache, const char *json, size_t sz)
{
    (void)sz;
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    cJSON *elements = cJSON_GetObjectItem(root, "elements");
    if (!cJSON_IsArray(elements)) { cJSON_Delete(root); return -1; }

    int ne = cJSON_GetArraySize(elements);
    for (int i = 0; i < ne; i++) {
        cJSON *el = cJSON_GetArrayItem(elements, i);
        cJSON *type_j = cJSON_GetObjectItem(el, "type");
        if (!cJSON_IsString(type_j)) continue;
        if (strcmp(type_j->valuestring, "way") != 0) continue;
        cJSON *tags = cJSON_GetObjectItem(el, "tags");
        int lid = classify_way(tags);
        if (lid < 0 || lid >= OSM_LAYER_COUNT) continue;
        cJSON *geom = cJSON_GetObjectItem(el, "geometry");
        push_way(&cache->layers[lid], geom);
    }
    cJSON_Delete(root);
    return 0;
}

/* Read a cached tile file into the cache layers. */
static int load_cached_tile(OsmCache *cache, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';
    int rc = parse_tile_json(cache, buf, got);
    free(buf);
    return rc;
}

/* Build the Overpass QL query for a single tile. */
static void build_query(int x, int y, char *out, size_t out_sz)
{
    double s, w, n, e;
    tile_bbox(x, y, &s, &w, &n, &e);
    snprintf(out, out_sz,
        "[out:json][timeout:25][bbox:%.6f,%.6f,%.6f,%.6f];"
        "("
        "way[\"highway\"];"
        "way[\"waterway\"][\"waterway\"!~\"^(drain|ditch)$\"];"
        "way[\"natural\"=\"water\"];"
        ");"
        "out geom;",
        s, w, n, e);
}

/* Synchronously fetch one tile from Overpass and write it to the disk
 * cache, then parse it into the layers. Returns 0 on success. */
static int fetch_tile(OsmCache *cache, int x, int y)
{
    char query[1024];
    build_query(x, y, query, sizeof(query));

    /* Overpass accepts the query as POST body OR as ?data= url param.
     * http.c only exposes GET, so we URL-encode into a query string. */
    char url[4096];
    /* Simple encode: spaces -> %20, others pass through (the query uses
     * only ASCII punctuation that Overpass accepts verbatim, but to be
     * safe encode the few troublesome chars). */
    size_t ulen = snprintf(url, sizeof(url), "%s?data=", OVERPASS_URL);
    for (size_t i = 0; query[i] && ulen + 4 < sizeof(url); i++) {
        unsigned char c = (unsigned char)query[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            url[ulen++] = (char)c;
        } else {
            snprintf(url + ulen, sizeof(url) - ulen, "%%%02X", c);
            ulen += 3;
        }
    }
    url[ulen] = '\0';

    HttpBuffer buf = {0};
    snprintf(cache->status, sizeof(cache->status),
             "Fetching OSM tile %d/%d/%d...", OSM_TILE_Z, x, y);
    if (http_get(url, &buf) != 0) {
        http_buffer_free(&buf);
        cache->last_error = 1;
        snprintf(cache->status, sizeof(cache->status),
                 "OSM fetch failed for %d/%d", x, y);
        return -1;
    }

    /* Save to disk cache. */
    char path[512];
    tile_path(x, y, path, sizeof(path));
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(buf.data, 1, buf.size, fp);
        fclose(fp);
    }

    /* Parse into layers. */
    int rc = parse_tile_json(cache, buf.data, buf.size);
    http_buffer_free(&buf);
    if (rc == 0) {
        snprintf(cache->status, sizeof(cache->status),
                 "Loaded OSM tile %d/%d/%d", OSM_TILE_Z, x, y);
    }
    return rc;
}

/* ---------------------------------------------------------------------------
 * Public API.
 * -------------------------------------------------------------------------*/

void osm_init(OsmCache *cache)
{
    memset(cache, 0, sizeof(*cache));
    for (int i = 0; i < OSM_LAYER_COUNT; i++) geolayer_init(&cache->layers[i]);
    memset(g_tiles, 0, sizeof(g_tiles));
    g_tile_count = 0;
}

void osm_free(OsmCache *cache)
{
    for (int i = 0; i < OSM_LAYER_COUNT; i++) geolayer_free(&cache->layers[i]);
    memset(cache, 0, sizeof(*cache));
    memset(g_tiles, 0, sizeof(g_tiles));
    g_tile_count = 0;
}

void osm_request_viewport(OsmCache *cache, MapView *mv)
{
    /* Only at zoom where OSM detail actually helps. Below zoom 9 the tile
     * grid gets absurd and Natural Earth still looks fine. */
    if (mv->zoom < 9) return;

    /* Determine viewport lat/lon bbox (reuses the map's projection math). */
    double lat_tl, lon_tl, lat_br, lon_br;
    map_screen_to_latlon(mv, 0, 0, &lat_tl, &lon_tl);
    map_screen_to_latlon(mv, mv->screen_w - 1, mv->screen_h - 1, &lat_br, &lon_br);
    double lat_min = lat_tl < lat_br ? lat_tl : lat_br;
    double lat_max = lat_tl > lat_br ? lat_tl : lat_br;
    double lon_min = lon_tl < lon_br ? lon_tl : lon_br;
    double lon_max = lon_tl > lon_br ? lon_tl : lon_br;

    int x0, y0, x1, y1;
    latlon_to_tile(lat_max, lon_min, &x0, &y0);
    latlon_to_tile(lat_min, lon_max, &x1, &y1);
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }

    /* Cap the tile request rectangle to avoid asking for hundreds of tiles
     * if the viewport somehow covers a huge area at zoom 9. */
    int tiles = (x1 - x0 + 1) * (y1 - y0 + 1);
    if (tiles > 32) return; /* too wide — skip until user zooms in */

    int pending = 0;
    for (int x = x0; x <= x1; x++) {
        for (int y = y0; y <= y1; y++) {
            int idx = tile_find(x, y);
            if (idx >= 0) {
                if (g_tiles[idx].status == TS_PENDING) pending++;
                continue;
            }
            /* First sighting of this tile. Check disk cache. */
            char path[512];
            tile_path(x, y, path, sizeof(path));
            if (file_exists(path)) {
                if (load_cached_tile(cache, path) == 0) {
                    tile_add(x, y, TS_LOADED);
                } else {
                    tile_add(x, y, TS_FAILED);
                }
            } else {
                tile_add(x, y, TS_PENDING);
                pending++;
            }
        }
    }
    cache->tiles_pending = pending;
}

int osm_tick(OsmCache *cache)
{
    /* Fetch at most one pending tile per call to stay responsive. */
    for (int i = 0; i < g_tile_count; i++) {
        if (g_tiles[i].status == TS_PENDING) {
            int x = g_tiles[i].x, y = g_tiles[i].y;
            int rc = fetch_tile(cache, x, y);
            g_tiles[i].status = (rc == 0) ? TS_LOADED : TS_FAILED;
            /* Recount pending. */
            int p = 0;
            for (int j = 0; j < g_tile_count; j++)
                if (g_tiles[j].status == TS_PENDING) p++;
            cache->tiles_pending = p;
            return 1;
        }
    }
    return 0;
}

int osm_is_busy(OsmCache *cache) { return cache->tiles_pending > 0; }
