#ifndef MVT_H
#define MVT_H

#include <stddef.h>
#include "osm.h"

/* ---------------------------------------------------------------------------
 * Mapbox Vector Tile decoder.
 *
 * MVT is a protobuf format (schema: vector_tile.proto v2.1) used by most
 * vector map tilesets — including the OpenMapTiles schema we're using.
 *
 * A tile contains Layers, each Layer contains Features, each Feature has
 * a geometry (encoded as "command integers" — move_to/line_to/close) and
 * a set of key/value tag pairs (indexed into the layer's keys[]/values[]
 * tables).
 *
 * Tile-local geometry coordinates are integers in [0, extent) (usually
 * extent = 4096). The decoder converts them to WGS84 lon/lat using the
 * tile's z/x/y position in the slippy grid, then appends each feature to
 * the appropriate OsmLayerId in the OsmCache.
 *
 * Tile blobs in MBTiles are gzip-compressed — mvt_decode_tile handles the
 * gunzip via zlib internally.
 * ---------------------------------------------------------------------*/

/* Decode one tile blob (possibly gzip-compressed) and append every
 * relevant feature to `cache->layers[...]`. Returns 0 on success. */
int mvt_decode_tile(OsmCache *cache,
                    int z, int x, int y,
                    const unsigned char *data, size_t size);

#endif /* MVT_H */
