#ifndef MAP_H
#define MAP_H

#ifdef _MAP_NEEDS_CURSES
#include <curses.h>
#endif

/* Forward declare WINDOW if curses not included */
#ifndef _MAP_NEEDS_CURSES
typedef void WINDOW;
#endif

/* Map viewport state */
typedef struct {
    double center_lat;
    double center_lon;
    int    zoom;          /* OSM zoom level 2..18 */
    int    screen_w;      /* terminal cols available for map */
    int    screen_h;      /* terminal rows available for map */
} MapView;

/* A point on the route to be overlaid on the map */
typedef struct {
    double lat;
    double lon;
} MapPoint;

/* Route overlay data */
typedef struct {
    MapPoint *points;
    int       count;
    int       capacity;
    /* Origin & destination markers */
    double orig_lat, orig_lon;
    double dest_lat, dest_lon;
    int    has_route;
} RouteOverlay;

/* Initialize default map view (centered on USA) */
void map_init(MapView *mv);

/* Free all cached vector data. Call once at program exit. */
void map_shutdown(void);

/* Drive background work — fetches one OSM tile from the queue if any.
 * Returns 1 if something changed and the caller should re-render. */
int map_tick(void);

/* Human-readable status from the background work (e.g. "Fetching OSM..."). */
const char *map_status(void);

/* Render the ASCII map into the given curses window.
   If overlay is non-NULL and has_route, draw the route on top. */
void map_render(WINDOW *win, MapView *mv, RouteOverlay *overlay);

/* Pan the map by dx, dy in character-cell units */
void map_pan(MapView *mv, int dx, int dy);

/* Zoom in/out. Returns new zoom level. */
int map_zoom_in(MapView *mv);
int map_zoom_out(MapView *mv);

/* Center the map on a given lat/lon */
void map_center_on(MapView *mv, double lat, double lon);

/* Convert screen position (col, row in map window) to lat/lon */
void map_screen_to_latlon(MapView *mv, int col, int row, double *lat, double *lon);

/* Convert lat/lon to screen position. Returns 1 if visible, 0 if off-screen. */
int map_latlon_to_screen(MapView *mv, double lat, double lon, int *col, int *row);

/* RouteOverlay helpers */
void route_overlay_init(RouteOverlay *ro);
void route_overlay_free(RouteOverlay *ro);
void route_overlay_add_point(RouteOverlay *ro, double lat, double lon);
void route_overlay_clear(RouteOverlay *ro);

#endif /* MAP_H */