#include "geodata.h"
#include "cJSON.h"
#include "http.h"

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

/* ---------- file table ----------
 * Natural Earth 1.x GeoJSON from nvkelso/natural-earth-vector. We pick the
 * minimum set needed for a recognizable basemap. */

typedef struct {
    GeoLod      lod;
    GeoLayerId  layer;
    const char *filename;   /* local file name under data/ */
    const char *url;        /* remote URL to fetch on first run */
    const char *description;
} FileEntry;

#define NE_BASE "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"

static const FileEntry FILES[] = {
    /* --- 110m (world overview, zoom 2-4) --- */
    {LOD_110M, LAYER_LAND,      "ne_110m_land.geojson",
     NE_BASE "ne_110m_land.geojson", "world land polygons (low-res)"},
    {LOD_110M, LAYER_LAKES,     "ne_110m_lakes.geojson",
     NE_BASE "ne_110m_lakes.geojson", "world lakes (low-res)"},
    {LOD_110M, LAYER_COASTLINE, "ne_110m_coastline.geojson",
     NE_BASE "ne_110m_coastline.geojson", "world coastlines (low-res)"},
    {LOD_110M, LAYER_COUNTRIES, "ne_110m_admin_0_boundary_lines_land.geojson",
     NE_BASE "ne_110m_admin_0_boundary_lines_land.geojson", "country borders (low-res)"},
    {LOD_110M, LAYER_POP_PLACES, "ne_110m_populated_places_simple.geojson",
     NE_BASE "ne_110m_populated_places_simple.geojson", "major cities (low-res)"},

    /* --- 50m (continental, zoom 5-7) --- */
    {LOD_50M, LAYER_LAND,      "ne_50m_land.geojson",
     NE_BASE "ne_50m_land.geojson", "world land polygons (medium-res)"},
    {LOD_50M, LAYER_LAKES,     "ne_50m_lakes.geojson",
     NE_BASE "ne_50m_lakes.geojson", "lakes (medium-res)"},
    {LOD_50M, LAYER_COASTLINE, "ne_50m_coastline.geojson",
     NE_BASE "ne_50m_coastline.geojson", "coastlines (medium-res)"},
    {LOD_50M, LAYER_COUNTRIES, "ne_50m_admin_0_boundary_lines_land.geojson",
     NE_BASE "ne_50m_admin_0_boundary_lines_land.geojson", "country borders (medium-res)"},
    {LOD_50M, LAYER_STATES,    "ne_50m_admin_1_states_provinces_lines.geojson",
     NE_BASE "ne_50m_admin_1_states_provinces_lines.geojson", "state/province borders (medium-res)"},
    {LOD_50M, LAYER_RIVERS,    "ne_50m_rivers_lake_centerlines.geojson",
     NE_BASE "ne_50m_rivers_lake_centerlines.geojson", "major rivers (medium-res)"},
    {LOD_50M, LAYER_POP_PLACES, "ne_50m_populated_places_simple.geojson",
     NE_BASE "ne_50m_populated_places_simple.geojson", "cities (medium-res)"},

    /* --- 10m (regional / city, zoom 8+) --- */
    {LOD_10M, LAYER_LAND,      "ne_10m_land.geojson",
     NE_BASE "ne_10m_land.geojson", "world land polygons (high-res)"},
    {LOD_10M, LAYER_LAKES,     "ne_10m_lakes.geojson",
     NE_BASE "ne_10m_lakes.geojson", "lakes (high-res)"},
    {LOD_10M, LAYER_COASTLINE, "ne_10m_coastline.geojson",
     NE_BASE "ne_10m_coastline.geojson", "coastlines (high-res)"},
    {LOD_10M, LAYER_COUNTRIES, "ne_10m_admin_0_boundary_lines_land.geojson",
     NE_BASE "ne_10m_admin_0_boundary_lines_land.geojson", "country borders (high-res)"},
    {LOD_10M, LAYER_STATES,    "ne_10m_admin_1_states_provinces_lines.geojson",
     NE_BASE "ne_10m_admin_1_states_provinces_lines.geojson", "state/province borders (high-res)"},
    {LOD_10M, LAYER_RIVERS,    "ne_10m_rivers_lake_centerlines.geojson",
     NE_BASE "ne_10m_rivers_lake_centerlines.geojson", "rivers (high-res)"},
    {LOD_10M, LAYER_POP_PLACES, "ne_10m_populated_places_simple.geojson",
     NE_BASE "ne_10m_populated_places_simple.geojson", "cities (high-res)"},
};
static const int NFILES = (int)(sizeof(FILES) / sizeof(FILES[0]));

/* ---------- layer helpers ---------- */

void geolayer_init(GeoLayer *layer) { memset(layer, 0, sizeof(*layer)); }

void geolayer_free(GeoLayer *layer)
{
    for (int i = 0; i < layer->nfeatures; i++) {
        GeoFeature *f = &layer->features[i];
        for (int r = 0; r < f->nrings; r++) free(f->rings[r].coords);
        free(f->rings);
    }
    free(layer->features);
    memset(layer, 0, sizeof(*layer));
}

void geolayer_add(GeoLayer *layer, GeoFeature feat)
{
    if (layer->nfeatures >= layer->capacity) {
        int nc = layer->capacity ? layer->capacity * 2 : 16;
        GeoFeature *tmp = realloc(layer->features, nc * sizeof(GeoFeature));
        if (!tmp) return;
        layer->features = tmp;
        layer->capacity = nc;
    }
    layer->features[layer->nfeatures++] = feat;
}

/* ---------- dataset lifecycle ---------- */

void geodata_init(GeoDataset *ds) { memset(ds, 0, sizeof(*ds)); }

void geodata_free(GeoDataset *ds)
{
    for (int l = 0; l < LOD_COUNT; l++)
        for (int id = 0; id < LAYER_COUNT; id++)
            geolayer_free(&ds->layers[l][id]);
}

GeoLod geodata_lod_for_zoom(int zoom)
{
    if (zoom <= 4)  return LOD_110M;
    if (zoom <= 7)  return LOD_50M;
    return LOD_10M;
}

/* ---------- GeoJSON parsing ---------- */

/* Read an entire file into a malloc'd NUL-terminated buffer. */
static char *read_file(const char *path, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_size) *out_size = n;
    return buf;
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void bbox_init(GeoBBox *b)
{
    b->min_lat =  90.0f;  b->max_lat = -90.0f;
    b->min_lon = 180.0f;  b->max_lon = -180.0f;
}

static void bbox_add(GeoBBox *b, float lon, float lat)
{
    if (lat < b->min_lat) b->min_lat = lat;
    if (lat > b->max_lat) b->max_lat = lat;
    if (lon < b->min_lon) b->min_lon = lon;
    if (lon > b->max_lon) b->max_lon = lon;
}

static void bbox_merge(GeoBBox *dst, const GeoBBox *src)
{
    if (src->min_lat < dst->min_lat) dst->min_lat = src->min_lat;
    if (src->max_lat > dst->max_lat) dst->max_lat = src->max_lat;
    if (src->min_lon < dst->min_lon) dst->min_lon = src->min_lon;
    if (src->max_lon > dst->max_lon) dst->max_lon = src->max_lon;
}

/* Parse a GeoJSON ring: [[lon,lat],[lon,lat],...] into a GeoRing. */
static int parse_ring(cJSON *arr, GeoRing *ring, GeoBBox *bb)
{
    if (!cJSON_IsArray(arr)) return -1;
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return -1;
    float *coords = malloc((size_t)n * 2 * sizeof(float));
    if (!coords) return -1;
    int ci = 0;
    for (int i = 0; i < n; i++) {
        cJSON *pt = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) continue;
        cJSON *jlon = cJSON_GetArrayItem(pt, 0);
        cJSON *jlat = cJSON_GetArrayItem(pt, 1);
        if (!cJSON_IsNumber(jlon) || !cJSON_IsNumber(jlat)) continue;
        float lon = (float)jlon->valuedouble;
        float lat = (float)jlat->valuedouble;
        coords[ci * 2 + 0] = lon;
        coords[ci * 2 + 1] = lat;
        bbox_add(bb, lon, lat);
        ci++;
    }
    if (ci == 0) { free(coords); return -1; }
    ring->coords = coords;
    ring->npoints = ci;
    return 0;
}

/* Append features extracted from one GeoJSON geometry to the layer.
 * A single GeoJSON "Feature" may contain Multi* geometries — we flatten
 * each sub-geometry into its own GeoFeature so bbox culling stays tight. */
static void append_from_geometry(GeoLayer *layer, cJSON *geom,
                                 const char *name, int rank)
{
    if (!geom) return;
    cJSON *type_j = cJSON_GetObjectItem(geom, "type");
    cJSON *coords = cJSON_GetObjectItem(geom, "coordinates");
    if (!cJSON_IsString(type_j) || !coords) return;
    const char *t = type_j->valuestring;

    /* Helper to fill name/rank on a freshly built feature. */
    #define FILL_META(f) do { \
        if (name) { strncpy((f).name, name, sizeof((f).name)-1); } \
        (f).rank = rank; \
    } while (0)

    if (strcmp(t, "Polygon") == 0) {
        /* coords: [ ring, ring, ... ] */
        int nr = cJSON_GetArraySize(coords);
        if (nr <= 0) return;
        GeoFeature f = {0};
        f.type = GEOM_POLYGON;
        bbox_init(&f.bbox);
        f.rings = calloc((size_t)nr, sizeof(GeoRing));
        if (!f.rings) return;
        for (int i = 0; i < nr; i++) {
            cJSON *ring = cJSON_GetArrayItem(coords, i);
            if (parse_ring(ring, &f.rings[f.nrings], &f.bbox) == 0) f.nrings++;
        }
        if (f.nrings == 0) { free(f.rings); return; }
        FILL_META(f);
        geolayer_add(layer, f);
    }
    else if (strcmp(t, "MultiPolygon") == 0) {
        /* coords: [ polygon, polygon, ... ] where each polygon is [ring,...] */
        int np = cJSON_GetArraySize(coords);
        for (int pi = 0; pi < np; pi++) {
            cJSON *poly = cJSON_GetArrayItem(coords, pi);
            int nr = cJSON_GetArraySize(poly);
            if (nr <= 0) continue;
            GeoFeature f = {0};
            f.type = GEOM_POLYGON;
            bbox_init(&f.bbox);
            f.rings = calloc((size_t)nr, sizeof(GeoRing));
            if (!f.rings) continue;
            for (int i = 0; i < nr; i++) {
                cJSON *ring = cJSON_GetArrayItem(poly, i);
                if (parse_ring(ring, &f.rings[f.nrings], &f.bbox) == 0) f.nrings++;
            }
            if (f.nrings == 0) { free(f.rings); continue; }
            FILL_META(f);
            geolayer_add(layer, f);
        }
    }
    else if (strcmp(t, "LineString") == 0) {
        GeoFeature f = {0};
        f.type = GEOM_LINESTRING;
        bbox_init(&f.bbox);
        f.rings = calloc(1, sizeof(GeoRing));
        if (!f.rings) return;
        if (parse_ring(coords, &f.rings[0], &f.bbox) != 0) { free(f.rings); return; }
        f.nrings = 1;
        FILL_META(f);
        geolayer_add(layer, f);
    }
    else if (strcmp(t, "MultiLineString") == 0) {
        int nl = cJSON_GetArraySize(coords);
        for (int i = 0; i < nl; i++) {
            cJSON *line = cJSON_GetArrayItem(coords, i);
            GeoFeature f = {0};
            f.type = GEOM_LINESTRING;
            bbox_init(&f.bbox);
            f.rings = calloc(1, sizeof(GeoRing));
            if (!f.rings) continue;
            if (parse_ring(line, &f.rings[0], &f.bbox) != 0) { free(f.rings); continue; }
            f.nrings = 1;
            FILL_META(f);
            geolayer_add(layer, f);
        }
    }
    else if (strcmp(t, "Point") == 0) {
        if (!cJSON_IsArray(coords) || cJSON_GetArraySize(coords) < 2) return;
        cJSON *jlon = cJSON_GetArrayItem(coords, 0);
        cJSON *jlat = cJSON_GetArrayItem(coords, 1);
        if (!cJSON_IsNumber(jlon) || !cJSON_IsNumber(jlat)) return;
        GeoFeature f = {0};
        f.type = GEOM_POINT;
        f.rings = calloc(1, sizeof(GeoRing));
        if (!f.rings) return;
        f.rings[0].coords = malloc(2 * sizeof(float));
        f.rings[0].npoints = 1;
        f.rings[0].coords[0] = (float)jlon->valuedouble;
        f.rings[0].coords[1] = (float)jlat->valuedouble;
        bbox_init(&f.bbox);
        bbox_add(&f.bbox, f.rings[0].coords[0], f.rings[0].coords[1]);
        f.nrings = 1;
        FILL_META(f);
        geolayer_add(layer, f);
    }

    #undef FILL_META
}

int geodata_load_file(const char *path, GeoLayer *layer, GeoLayerId hint)
{
    (void)hint;
    size_t sz = 0;
    char *buf = read_file(path, &sz);
    if (!buf) {
        fprintf(stderr, "[geodata] missing: %s\n", path);
        return -1;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        fprintf(stderr, "[geodata] parse failed: %s\n", path);
        return -1;
    }

    cJSON *type_j = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_j)) { cJSON_Delete(root); return -1; }

    int before = layer->nfeatures;

    if (strcmp(type_j->valuestring, "FeatureCollection") == 0) {
        cJSON *feats = cJSON_GetObjectItem(root, "features");
        if (!cJSON_IsArray(feats)) { cJSON_Delete(root); return -1; }
        int nf = cJSON_GetArraySize(feats);
        for (int i = 0; i < nf; i++) {
            cJSON *feat = cJSON_GetArrayItem(feats, i);
            cJSON *geom = cJSON_GetObjectItem(feat, "geometry");
            cJSON *props = cJSON_GetObjectItem(feat, "properties");
            const char *name = NULL;
            int rank = 0;
            if (cJSON_IsObject(props)) {
                cJSON *nm = cJSON_GetObjectItem(props, "name");
                if (cJSON_IsString(nm)) name = nm->valuestring;
                cJSON *sr = cJSON_GetObjectItem(props, "scalerank");
                if (cJSON_IsNumber(sr)) rank = sr->valueint;
            }
            append_from_geometry(layer, geom, name, rank);
        }
    } else if (strcmp(type_j->valuestring, "Feature") == 0) {
        cJSON *geom = cJSON_GetObjectItem(root, "geometry");
        append_from_geometry(layer, geom, NULL, 0);
    } else {
        append_from_geometry(layer, root, NULL, 0);
    }

    cJSON_Delete(root);
    fprintf(stderr, "[geodata] loaded %s (%d features)\n",
            path, layer->nfeatures - before);
    return 0;
}

/* ---------- bootstrap & LOD loading ---------- */

static void data_path(const FileEntry *fe, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "data/%s", fe->filename);
}

int geodata_bootstrap_files(GeoLod lod)
{
    /* Ensure data/ directory exists. */
    struct stat st;
    if (stat("data", &st) != 0) {
        if (MKDIR("data") != 0) {
            fprintf(stderr, "[geodata] cannot create data/ directory\n");
            return -1;
        }
    }

    int missing = 0;
    for (int i = 0; i < NFILES; i++) {
        if (FILES[i].lod != lod) continue;
        char path[512];
        data_path(&FILES[i], path, sizeof(path));
        if (!file_exists(path)) missing++;
    }
    if (missing == 0) return 0;

    fprintf(stderr, "[geodata] fetching %d missing Natural Earth file(s) for LOD %d...\n",
            missing, (int)lod);

    for (int i = 0; i < NFILES; i++) {
        if (FILES[i].lod != lod) continue;
        char path[512];
        data_path(&FILES[i], path, sizeof(path));
        if (file_exists(path)) continue;

        fprintf(stderr, "[geodata]   GET %s\n", FILES[i].url);
        HttpBuffer buf = {0};
        if (http_get(FILES[i].url, &buf) != 0) {
            fprintf(stderr, "[geodata]   FAILED: %s\n", FILES[i].filename);
            /* non-fatal — the layer just stays empty */
            continue;
        }
        FILE *fp = fopen(path, "wb");
        if (!fp) {
            http_buffer_free(&buf);
            fprintf(stderr, "[geodata]   cannot write %s\n", path);
            continue;
        }
        fwrite(buf.data, 1, buf.size, fp);
        fclose(fp);
        http_buffer_free(&buf);
        fprintf(stderr, "[geodata]   saved %s\n", path);
    }
    return 0;
}

int geodata_ensure_lod(GeoDataset *ds, GeoLod lod)
{
    if (lod < 0 || lod >= LOD_COUNT) return -1;
    if (ds->lod_loaded[lod]) return 0;

    /* Fetch files if missing — non-fatal on per-file failure. */
    geodata_bootstrap_files(lod);

    for (int i = 0; i < NFILES; i++) {
        if (FILES[i].lod != lod) continue;
        char path[512];
        data_path(&FILES[i], path, sizeof(path));
        if (!file_exists(path)) continue;
        geodata_load_file(path, &ds->layers[lod][FILES[i].layer], FILES[i].layer);
    }

    ds->lod_loaded[lod] = 1;
    return 0;
}
