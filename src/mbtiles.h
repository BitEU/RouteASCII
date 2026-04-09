#ifndef MBTILES_H
#define MBTILES_H

#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Thin sqlite3 wrapper for reading MBTiles files.
 *
 * An MBTiles file is a sqlite database with a `tiles(zoom_level,
 * tile_column, tile_row, tile_data)` table. Tiles are stored in TMS
 * coordinates (Y=0 at the bottom of the world), which is the opposite of
 * the slippy-map XYZ convention — we handle the flip here so callers can
 * just pass XYZ coordinates.
 *
 * Tile blobs in modern MBTiles files are gzip-compressed MVT. We return
 * the raw blob; the caller (mvt.c) is responsible for gunzipping and
 * decoding.
 * ---------------------------------------------------------------------*/

typedef struct Mbtiles Mbtiles;

/* Open a .mbtiles file. Returns NULL on failure. */
Mbtiles *mbtiles_open(const char *path);

/* Close and free. */
void mbtiles_close(Mbtiles *m);

/* Return min/max zoom levels present in the file. -1 if unknown. */
int mbtiles_minzoom(const Mbtiles *m);
int mbtiles_maxzoom(const Mbtiles *m);

/* Fetch a tile's raw blob by XYZ coords. On success sets *out_data to a
 * malloc'd buffer (caller frees) and *out_size to its length, returns 0.
 * On miss or error returns -1 and *out_data is NULL. */
int mbtiles_get_tile(Mbtiles *m, int z, int x, int y,
                     unsigned char **out_data, size_t *out_size);

#endif /* MBTILES_H */
