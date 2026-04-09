#define _MAP_NEEDS_CURSES
#include "map.h"
#include "geo.h"
#include "geodata.h"
#include "render.h"
#include "ui.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MIN_ZOOM 2
#define MAX_ZOOM 16

/* Global dataset held by the module — one per process. The alternative is
 * to thread it through every map_* call, but the render routines already
 * take a MapView pointer; adding a second pointer to the public API would
 * ripple through main.c and the route code. Keep it file-local. */
static GeoDataset g_dataset;
static int        g_dataset_initialized = 0;

static void ensure_dataset(MapView *mv)
{
    if (!g_dataset_initialized) {
        geodata_init(&g_dataset);
        g_dataset_initialized = 1;
        /* Load the low-res LOD up front so the first frame has something
         * to draw even if the user hasn't panned yet. */
        geodata_ensure_lod(&g_dataset, LOD_110M);
    }
    GeoLod need = geodata_lod_for_zoom(mv->zoom);
    if (!g_dataset.lod_loaded[need]) {
        geodata_ensure_lod(&g_dataset, need);
    }
}

void map_init(MapView *mv)
{
    mv->center_lat = 39.8;
    mv->center_lon = -98.5;
    mv->zoom = 4;
    mv->screen_w = 80;
    mv->screen_h = 24;
}

void map_render(WINDOW *win, MapView *mv, RouteOverlay *overlay)
{
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    mv->screen_w = max_x;
    mv->screen_h = max_y;

    ensure_dataset(mv);
    GeoLod lod = geodata_lod_for_zoom(mv->zoom);
    /* If the requested LOD failed to load (no network, say), fall back to
     * whatever we do have. */
    if (!g_dataset.lod_loaded[lod]) {
        for (int l = 0; l < LOD_COUNT; l++) {
            if (g_dataset.lod_loaded[l]) { lod = (GeoLod)l; break; }
        }
    }

    /* --- 1. Clear to water --- */
    render_clear_water(win);

    /* --- 1b. Graticule on the water (land will overwrite it) --- */
    render_graticule(win, mv);

    /* --- 2. Fill land --- */
    LayerStyle land_style = {
        .cp = CP_LAND,  .attr = 0,
        .fill_ch = '.', .stroke_ch = 0,  .use_slope = 0
    };
    render_polygon_layer(win, mv, &g_dataset.layers[lod][LAYER_LAND], &land_style);

    /* --- 3. Carve lakes back out as water --- */
    LayerStyle lake_style = {
        .cp = CP_WATER, .attr = 0,
        .fill_ch = ' ', .stroke_ch = 0, .use_slope = 0
    };
    render_polygon_layer(win, mv, &g_dataset.layers[lod][LAYER_LAKES], &lake_style);

    /* --- 4. Rivers (linestrings) --- */
    LayerStyle river_style = {
        .cp = CP_WATER, .attr = A_DIM,
        .fill_ch = 0,   .stroke_ch = '~', .use_slope = 0
    };
    render_line_layer(win, mv, &g_dataset.layers[lod][LAYER_RIVERS], &river_style);

    /* --- 5. State / province borders (dim dotted) --- */
    LayerStyle state_style = {
        .cp = CP_BORDER, .attr = A_DIM,
        .fill_ch = 0,    .stroke_ch = ':', .use_slope = 0
    };
    render_line_layer(win, mv, &g_dataset.layers[lod][LAYER_STATES], &state_style);

    /* --- 6. Country borders (brighter dashed) --- */
    LayerStyle country_style = {
        .cp = CP_BORDER, .attr = A_BOLD,
        .fill_ch = 0,    .stroke_ch = '+', .use_slope = 0
    };
    render_line_layer(win, mv, &g_dataset.layers[lod][LAYER_COUNTRIES], &country_style);

    /* --- 7. Coastlines (sharp, slope-aware) --- */
    LayerStyle coast_style = {
        .cp = CP_LAND,  .attr = A_BOLD,
        .fill_ch = 0,   .stroke_ch = '#', .use_slope = 1
    };
    render_line_layer(win, mv, &g_dataset.layers[lod][LAYER_COASTLINE], &coast_style);

    /* --- 9. OSRM route overlay --- */
    if (overlay && overlay->has_route && overlay->count > 0) {
        render_polyline_points(win, mv, overlay->points, overlay->count,
                               '*', CP_ROUTE, A_BOLD);

        int sc, sr;
        if (map_latlon_to_screen(mv, overlay->orig_lat, overlay->orig_lon,
                                 &sc, &sr)) {
            mvwaddch(win, sr, sc, 'A' | COLOR_PAIR(CP_MARKER) | A_BOLD);
        }
        if (map_latlon_to_screen(mv, overlay->dest_lat, overlay->dest_lon,
                                 &sc, &sr)) {
            mvwaddch(win, sr, sc, 'B' | COLOR_PAIR(CP_MARKER) | A_BOLD);
        }
    }

    /* --- 10. Cities (on top of everything except route markers) --- */
    render_places_layer(win, mv, &g_dataset.layers[lod][LAYER_POP_PLACES]);

    wrefresh(win);
}

/* ---------- pan / zoom / conversions (unchanged from before) ---------- */

static void screen_cell_to_latlon(MapView *mv, int col, int row,
                                  double *lat, double *lon)
{
    double center_px, center_py;
    geo_latlon_to_pixel(mv->center_lat, mv->center_lon, mv->zoom,
                        &center_px, &center_py);

    double scale = 256.0 / (double)(1 << (18 - mv->zoom));
    if (scale < 1.0) scale = 1.0;
    double char_w = scale * 2.0;
    double char_h = scale * 4.0;

    double px = center_px + (col - mv->screen_w / 2.0) * char_w;
    double py = center_py + (row - mv->screen_h / 2.0) * char_h;

    int n = 1 << mv->zoom;
    *lon = px / (n * 256.0) * 360.0 - 180.0;
    double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * py / (n * 256.0))));
    *lat = lat_rad * 180.0 / M_PI;
}

void map_pan(MapView *mv, int dx, int dy)
{
    double lat1, lon1, lat2, lon2;
    screen_cell_to_latlon(mv, mv->screen_w / 2, mv->screen_h / 2, &lat1, &lon1);
    screen_cell_to_latlon(mv, mv->screen_w / 2 + 1, mv->screen_h / 2 + 1, &lat2, &lon2);

    double dlon = (lon2 - lon1) * dx * 3;
    double dlat = (lat2 - lat1) * dy * 3;

    mv->center_lon += dlon;
    mv->center_lat -= dlat;

    if (mv->center_lat > 85.0) mv->center_lat = 85.0;
    if (mv->center_lat < -85.0) mv->center_lat = -85.0;
    while (mv->center_lon > 180.0) mv->center_lon -= 360.0;
    while (mv->center_lon < -180.0) mv->center_lon += 360.0;
}

int map_zoom_in(MapView *mv)  { if (mv->zoom < MAX_ZOOM) mv->zoom++; return mv->zoom; }
int map_zoom_out(MapView *mv) { if (mv->zoom > MIN_ZOOM) mv->zoom--; return mv->zoom; }

void map_center_on(MapView *mv, double lat, double lon)
{
    mv->center_lat = lat;
    mv->center_lon = lon;
}

void map_screen_to_latlon(MapView *mv, int col, int row, double *lat, double *lon)
{
    screen_cell_to_latlon(mv, col, row, lat, lon);
}

int map_latlon_to_screen(MapView *mv, double lat, double lon, int *col, int *row)
{
    double center_px, center_py;
    geo_latlon_to_pixel(mv->center_lat, mv->center_lon, mv->zoom,
                        &center_px, &center_py);

    double pt_px, pt_py;
    geo_latlon_to_pixel(lat, lon, mv->zoom, &pt_px, &pt_py);

    double scale = 256.0 / (double)(1 << (18 - mv->zoom));
    if (scale < 1.0) scale = 1.0;
    double char_w = scale * 2.0;
    double char_h = scale * 4.0;

    *col = (int)(mv->screen_w / 2.0 + (pt_px - center_px) / char_w);
    *row = (int)(mv->screen_h / 2.0 + (pt_py - center_py) / char_h);

    return (*col >= 0 && *col < mv->screen_w && *row >= 0 && *row < mv->screen_h);
}

/* ---------- route overlay (unchanged) ---------- */

void route_overlay_init(RouteOverlay *ro) { memset(ro, 0, sizeof(*ro)); }

void route_overlay_free(RouteOverlay *ro)
{
    free(ro->points);
    memset(ro, 0, sizeof(*ro));
}

void route_overlay_add_point(RouteOverlay *ro, double lat, double lon)
{
    if (ro->count >= ro->capacity) {
        int new_cap = (ro->capacity == 0) ? 256 : ro->capacity * 2;
        MapPoint *tmp = realloc(ro->points, new_cap * sizeof(MapPoint));
        if (!tmp) return;
        ro->points = tmp;
        ro->capacity = new_cap;
    }
    ro->points[ro->count].lat = lat;
    ro->points[ro->count].lon = lon;
    ro->count++;
}

void route_overlay_clear(RouteOverlay *ro)
{
    ro->count = 0;
    ro->has_route = 0;
}

/* ---------- teardown hook ----------
 * Called from main.c at exit to free all vector data. We expose this via
 * a weak helper — main.c will call it if present. */
void map_shutdown(void)
{
    if (g_dataset_initialized) {
        geodata_free(&g_dataset);
        g_dataset_initialized = 0;
    }
}
