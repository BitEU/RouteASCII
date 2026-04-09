#ifndef GEODATA_H
#define GEODATA_H

#include <stddef.h>

/* ---------------------------------------------------------------------------
 * GeoData — in-memory vector geographic features loaded from Natural Earth
 * GeoJSON files. Polygons, linestrings and points, stored as float to halve
 * memory vs. double (float lat/lon has ~1m precision at the equator, which
 * is invisible at the character cell sizes we render).
 * -------------------------------------------------------------------------*/

typedef struct {
    float min_lat, min_lon;
    float max_lat, max_lon;
} GeoBBox;

/* A single ring of a polygon (outer or hole), or a single linestring. */
typedef struct {
    float *coords;   /* interleaved [lon0, lat0, lon1, lat1, ...] */
    int    npoints;
} GeoRing;

typedef enum {
    GEOM_POLYGON,     /* 1 or more rings; rings[0] outer, rest are holes */
    GEOM_LINESTRING,  /* single ring */
    GEOM_POINT        /* single point in rings[0].coords[0..1] */
} GeoGeomType;

typedef struct {
    GeoGeomType type;
    GeoBBox     bbox;
    GeoRing    *rings;
    int         nrings;
    /* Optional per-feature attributes we care about */
    int         rank;      /* for populated places: scalerank (lower = more prominent) */
    char        name[64];  /* for cities / countries */
} GeoFeature;

typedef struct {
    GeoFeature *features;
    int         nfeatures;
    int         capacity;
} GeoLayer;

/* Resolution bands. We load 110m at startup, 50m at startup, 10m lazily. */
typedef enum {
    LOD_110M = 0,
    LOD_50M  = 1,
    LOD_10M  = 2,
    LOD_COUNT
} GeoLod;

/* All the layers we know how to draw. */
typedef enum {
    LAYER_LAND = 0,           /* filled polygons */
    LAYER_LAKES,              /* filled polygons (drawn as water) */
    LAYER_COASTLINE,          /* linestrings */
    LAYER_COUNTRIES,          /* admin-0 border linestrings */
    LAYER_STATES,             /* admin-1 border linestrings */
    LAYER_RIVERS,             /* linestrings */
    LAYER_POP_PLACES,         /* points (cities) */
    LAYER_COUNT
} GeoLayerId;

typedef struct {
    /* layers[lod][id] — may be empty if file not present */
    GeoLayer layers[LOD_COUNT][LAYER_COUNT];
    int      lod_loaded[LOD_COUNT]; /* 1 if that LOD has been loaded */
} GeoDataset;

/* ---------- lifecycle ---------- */

/* Initialize an empty dataset. */
void geodata_init(GeoDataset *ds);

/* Free all feature memory. */
void geodata_free(GeoDataset *ds);

/* Ensure the given LOD is loaded into the dataset. Files are read from
 * the "data/" directory next to the exe. Returns 0 on success, -1 on failure.
 * Idempotent — safe to call every frame. */
int geodata_ensure_lod(GeoDataset *ds, GeoLod lod);

/* Ensure Natural Earth GeoJSON files exist in data/. If not, download them.
 * Returns 0 on success, -1 on failure. Prints progress to stderr. */
int geodata_bootstrap_files(GeoLod lod);

/* Load a single GeoJSON file into an existing layer. Appends features.
 * The layer id hints the geometry type expected. Returns 0 on success. */
int geodata_load_file(const char *path, GeoLayer *layer, GeoLayerId hint);

/* ---------- layer helpers ---------- */

void geolayer_init(GeoLayer *layer);
void geolayer_free(GeoLayer *layer);
void geolayer_add(GeoLayer *layer, GeoFeature feat);

/* Pick the best LOD for the current zoom level. */
GeoLod geodata_lod_for_zoom(int zoom);

#endif /* GEODATA_H */
