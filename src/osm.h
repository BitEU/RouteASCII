#ifndef OSM_H
#define OSM_H

#include "geodata.h"
#include "map.h"

/* ---------------------------------------------------------------------------
 * OSM vector cache — MBTiles edition.
 *
 * At high zoom Natural Earth runs out of detail. This module loads real
 * OpenStreetMap vector data from a local MBTiles file (produced by
 * tilemaker from a Geofabrik us.osm.pbf extract) using the standard
 * OpenMapTiles schema. Tiles are pulled from the sqlite DB on demand,
 * decoded from Mapbox Vector Tile format (protobuf), cached in memory
 * via a small LRU, and exposed as GeoLayers the existing renderer can
 * draw directly.
 *
 * No network. No Overpass. No missing tiles. Every viewport read is a
 * local sqlite query.
 *
 * The MBTiles file is expected at data/us.mbtiles. If it's missing the
 * module degrades gracefully — layers stay empty and a warning is shown
 * in the status bar.
 * -------------------------------------------------------------------------*/

typedef enum {
    /* Transportation — split by class so we can show only majors at low
     * zoom and fill in residential/service as the user zooms in. */
    OSM_LAYER_ROAD_MOTORWAY = 0,
    OSM_LAYER_ROAD_TRUNK,
    OSM_LAYER_ROAD_PRIMARY,
    OSM_LAYER_ROAD_SECONDARY,
    OSM_LAYER_ROAD_TERTIARY,
    OSM_LAYER_ROAD_MINOR,      /* residential, unclassified, living_street */
    OSM_LAYER_ROAD_SERVICE,    /* service, track */
    OSM_LAYER_ROAD_PATH,       /* path, footway, cycleway, steps (rendered only at extreme zoom) */

    /* Hydrography */
    OSM_LAYER_WATERWAY,        /* rivers, streams, canals */
    OSM_LAYER_WATER,           /* lakes, reservoirs, oceans — polygons */

    /* Places (cities, towns, villages) — used to augment / replace the
     * Natural Earth populated-places layer once we're zoomed in enough. */
    OSM_LAYER_PLACE,

    /* Boundaries */
    OSM_LAYER_BOUNDARY,

    OSM_LAYER_COUNT
} OsmLayerId;

/* Forward-declare TileEntry so the header can expose a tile iterator.
 * The struct definition lives in osm.c. */
typedef struct TileEntry TileEntry;

typedef struct {
    GeoLayer layers[OSM_LAYER_COUNT];
    int      tiles_pending;    /* legacy — always 0 now (sync loads) */
    int      last_error;
    char     status[128];
    int      mbtiles_ok;       /* 1 if the MBTiles file opened successfully */

    /* Direct tile iteration — avoids deep-copying features from the
     * LRU into cache->layers on every viewport change. Instead the
     * renderer walks the active tile list. */
    TileEntry **active_tiles;  /* array of pointers to LRU entries */
    int         active_count;
} OsmCache;

/* Initialize the cache. Opens the MBTiles file at data/us.mbtiles. Safe to
 * call even if the file is missing — the cache just stays empty and
 * mbtiles_ok is left 0. */
void osm_init(OsmCache *cache);

/* Free all memory and close the MBTiles handle. */
void osm_free(OsmCache *cache);

/* Rebuild the GeoLayers from whichever z=14 tiles cover the viewport.
 * Synchronous — just a few sqlite SELECTs + MVT decodes. Call once per
 * frame before rendering. */
void osm_request_viewport(OsmCache *cache, MapView *mv);

/* Iterate over the active tile set, calling render for each tile's layer.
 * This avoids the expensive deep-copy into cache->layers entirely. */
typedef void (*OsmLayerCallback)(const GeoLayer *layer, void *ctx);
void osm_foreach_layer(OsmCache *cache, OsmLayerId layer_id,
                       OsmLayerCallback cb, void *ctx);

/* No-op retained for API compatibility with the old Overpass path. */
int osm_tick(OsmCache *cache);

/* Always 0 now. */
int osm_is_busy(OsmCache *cache);

#endif /* OSM_H */
