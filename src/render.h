#ifndef RENDER_H
#define RENDER_H

#define _MAP_NEEDS_CURSES
#include <curses.h>
#include "geodata.h"
#include "map.h"

/* ---------------------------------------------------------------------------
 * render — vector-to-ASCII rasterization primitives. These operate in
 * character-cell screen coordinates, produced by projecting lat/lon through
 * the MapView. All primitives are bounds-checked against the window.
 * -------------------------------------------------------------------------*/

/* Style for a single layer — what char to use and what color pair. */
typedef struct {
    int cp;          /* curses color pair */
    int attr;        /* A_BOLD, A_DIM, etc. */
    int fill_ch;     /* for polygon interiors (0 = none) */
    int stroke_ch;   /* for polyline strokes and polygon outlines (0 = none) */
    int use_slope;   /* if non-zero, pick - | / \ from segment direction */
} LayerStyle;

/* Frame lifecycle. Call begin before any render_* calls, end after the
 * fills are done but before markers/labels — end flushes the braille
 * canvas onto the window. */
void render_begin_frame(WINDOW *win);
void render_end_frame(WINDOW *win);
void render_shutdown(void);

/* Clear the window to the water/background character. */
void render_clear_water(WINDOW *win);

/* Draw the lat/lon graticule faintly. */
void render_graticule(WINDOW *win, MapView *mv);

/* Fill every feature of a polygon layer. Features are bbox-culled against
 * the current viewport before projection. */
void render_polygon_layer(WINDOW *win, MapView *mv, const GeoLayer *layer,
                          const LayerStyle *style);

/* Stroke every feature of a linestring layer (or polygon outlines). */
void render_line_layer(WINDOW *win, MapView *mv, const GeoLayer *layer,
                       const LayerStyle *style);

/* Draw populated places as ranked markers. */
void render_places_layer(WINDOW *win, MapView *mv, const GeoLayer *layer);

/* Draw a single polyline from a raw lat/lon point array (used for the
 * OSRM route overlay). */
void render_polyline_points(WINDOW *win, MapView *mv,
                            const MapPoint *pts, int npts,
                            int ch, int cp, int attr);

#endif /* RENDER_H */
