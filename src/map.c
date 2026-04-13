#define _MAP_NEEDS_CURSES
#include "map.h"
#include "geo.h"
#include "geodata.h"
#include "render.h"
#include "osm.h"
#include "ui.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

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

static OsmCache   g_osm;
static int        g_osm_initialized = 0;

/* Callback context for osm_foreach_layer → render_*_layer. */
typedef struct {
    WINDOW     *win;
    MapView    *mv;
    LayerStyle *style;
} OsmRenderCtx;

static void osm_cb_poly(const GeoLayer *l, void *ctx) {
    OsmRenderCtx *c = ctx;
    render_polygon_layer(c->win, c->mv, l, c->style);
}
static void osm_cb_line(const GeoLayer *l, void *ctx) {
    OsmRenderCtx *c = ctx;
    render_line_layer(c->win, c->mv, l, c->style);
}
static void osm_cb_place(const GeoLayer *l, void *ctx) {
    OsmRenderCtx *c = ctx;
    render_places_layer(c->win, c->mv, l);
}

/* Background loading state for LOD_10M.  0 = idle, 1 = loading, 2 = done */
static volatile int g_10m_state = 0;

#ifdef _WIN32
static DWORD WINAPI bg_load_10m(LPVOID arg) {
    (void)arg;
    fprintf(stderr, "[geodata] background: loading 10m data...\n");
    geodata_ensure_lod(&g_dataset, LOD_10M);
    fprintf(stderr, "[geodata] background: 10m data ready\n");
    g_10m_state = 2;
    return 0;
}
#else
static void *bg_load_10m(void *arg) {
    (void)arg;
    fprintf(stderr, "[geodata] background: loading 10m data...\n");
    geodata_ensure_lod(&g_dataset, LOD_10M);
    fprintf(stderr, "[geodata] background: 10m data ready\n");
    g_10m_state = 2;
    return NULL;
}
#endif

static void ensure_dataset(MapView *mv)
{
    if (!g_dataset_initialized) {
        geodata_init(&g_dataset);
        g_dataset_initialized = 1;
        geodata_ensure_lod(&g_dataset, LOD_110M);
    }
    if (mv->zoom >= 5 && !g_dataset.lod_loaded[LOD_50M]) {
        geodata_ensure_lod(&g_dataset, LOD_50M);
    }
    /* Kick off 10m load in a background thread so the UI stays responsive.
     * The renderer uses 50m until 10m finishes loading. */
    if (mv->zoom >= 8 && g_10m_state == 0) {
        g_10m_state = 1;
#ifdef _WIN32
        CreateThread(NULL, 0, bg_load_10m, NULL, 0, NULL);
#else
        pthread_t t;
        pthread_create(&t, NULL, bg_load_10m, NULL);
        pthread_detach(t);
#endif
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

    /* Begin a frame — resets the braille canvas used for all strokes. */
    render_begin_frame(win);

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

    /* --- 7b. OSM vector detail from local MBTiles ---
     * Starting at zoom 6 (where Natural Earth starts showing its age) we
     * overlay real OSM road networks, waterways, and waterbodies from a
     * local us.mbtiles file generated offline by planetiler. If the file
     * is missing the layers will be empty and we degrade gracefully. */
    if (mv->zoom >= 6) {
        if (!g_osm_initialized) { osm_init(&g_osm); g_osm_initialized = 1; }
        osm_request_viewport(&g_osm, mv);

        /* Water polygons */
        LayerStyle osm_water_fill = {
            .cp = CP_WATER, .attr = 0,
            .fill_ch = ' ', .stroke_ch = 0, .use_slope = 0
        };
        OsmRenderCtx wctx = { win, mv, &osm_water_fill };
        osm_foreach_layer(&g_osm, OSM_LAYER_WATER, osm_cb_poly, &wctx);

        /* Rivers / streams */
        LayerStyle osm_waterway = {
            .cp = CP_WATER, .attr = A_DIM,
            .fill_ch = 0,   .stroke_ch = '~', .use_slope = 0
        };
        OsmRenderCtx rwctx = { win, mv, &osm_waterway };
        osm_foreach_layer(&g_osm, OSM_LAYER_WATERWAY, osm_cb_line, &rwctx);

        /* Per-zoom road class filter */
        int show_motorway  = mv->zoom >= 6;
        int show_trunk     = mv->zoom >= 6;
        int show_primary   = mv->zoom >= 8;
        int show_secondary = mv->zoom >= 9;
        int show_tertiary  = mv->zoom >= 11;
        int show_minor     = mv->zoom >= 12;
        int show_service   = mv->zoom >= 14;
        int show_path      = mv->zoom >= 15;

        LayerStyle st_motorway  = { .cp = CP_BORDER, .attr = A_BOLD,
                                    .fill_ch = 0, .stroke_ch = '=', .use_slope = 0 };
        LayerStyle st_trunk     = { .cp = CP_BORDER, .attr = A_BOLD,
                                    .fill_ch = 0, .stroke_ch = '=', .use_slope = 0 };
        LayerStyle st_primary   = { .cp = CP_BORDER, .attr = 0,
                                    .fill_ch = 0, .stroke_ch = '-', .use_slope = 1 };
        LayerStyle st_secondary = { .cp = CP_BORDER, .attr = 0,
                                    .fill_ch = 0, .stroke_ch = '-', .use_slope = 1 };
        LayerStyle st_tertiary  = { .cp = CP_GRID, .attr = 0,
                                    .fill_ch = 0, .stroke_ch = '-', .use_slope = 1 };
        LayerStyle st_minor     = { .cp = CP_GRID, .attr = A_DIM,
                                    .fill_ch = 0, .stroke_ch = '.', .use_slope = 0 };
        LayerStyle st_service   = { .cp = CP_GRID, .attr = A_DIM,
                                    .fill_ch = 0, .stroke_ch = '.', .use_slope = 0 };
        LayerStyle st_path      = { .cp = CP_GRID, .attr = A_DIM,
                                    .fill_ch = 0, .stroke_ch = ':', .use_slope = 0 };

        /* Draw order: least important first so majors sit on top. */
        OsmRenderCtx rctx = { win, mv, NULL };
        if (show_path)      { rctx.style = &st_path;      osm_foreach_layer(&g_osm, OSM_LAYER_ROAD_PATH, osm_cb_line, &rctx); }
        if (show_service)   { rctx.style = &st_service;    osm_foreach_layer(&g_osm, OSM_LAYER_ROAD_SERVICE, osm_cb_line, &rctx); }
        if (show_minor)     { rctx.style = &st_minor;      osm_foreach_layer(&g_osm, OSM_LAYER_ROAD_MINOR, osm_cb_line, &rctx); }
        if (show_tertiary)  { rctx.style = &st_tertiary;   osm_foreach_layer(&g_osm, OSM_LAYER_ROAD_TERTIARY, osm_cb_line, &rctx); }
        if (show_secondary) { rctx.style = &st_secondary;  osm_foreach_layer(&g_osm, OSM_LAYER_ROAD_SECONDARY, osm_cb_line, &rctx); }
        if (show_primary)   { rctx.style = &st_primary;    osm_foreach_layer(&g_osm, OSM_LAYER_ROAD_PRIMARY, osm_cb_line, &rctx); }
        if (show_trunk)     { rctx.style = &st_trunk;      osm_foreach_layer(&g_osm, OSM_LAYER_ROAD_TRUNK, osm_cb_line, &rctx); }
        if (show_motorway)  { rctx.style = &st_motorway;   osm_foreach_layer(&g_osm, OSM_LAYER_ROAD_MOTORWAY, osm_cb_line, &rctx); }
    }

    /* --- 9. OSRM route overlay (drawn into braille canvas too) --- */
    if (overlay && overlay->has_route && overlay->count > 0) {
        render_polyline_points(win, mv, overlay->points, overlay->count,
                               '*', CP_ROUTE, A_BOLD);
    }

    /* Flush all stroke dots from the braille canvas onto the window,
     * overlaying the fills we drew above. */
    render_end_frame(win);

    /* --- 10. Origin/dest markers and cities drawn AFTER braille flush
     * so their text glyphs sit on top of coastlines and route lines. --- */
    if (overlay && overlay->has_route && overlay->count > 0) {
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

    /* At zoom 9+ prefer the OMT `place` layer (every village with a
     * name) over Natural Earth's ~1000-city list. NE still wins below
     * that because OMT's place layer is sparse at low zoom levels. */
    if (mv->zoom >= 9 && g_osm_initialized && g_osm.mbtiles_ok) {
        OsmRenderCtx pctx = { win, mv, NULL };
        osm_foreach_layer(&g_osm, OSM_LAYER_PLACE, osm_cb_place, &pctx);
    } else {
        render_places_layer(win, mv, &g_dataset.layers[lod][LAYER_POP_PLACES]);
    }

    wrefresh(win);
}

/* ---------- pan / zoom / conversions (unchanged from before) ---------- */

static void screen_cell_to_latlon(MapView *mv, int col, int row,
                                  double *lat, double *lon)
{
    double center_px, center_py;
    geo_latlon_to_pixel(mv->center_lat, mv->center_lon, mv->zoom,
                        &center_px, &center_py);

    /* One cell = 2 sub-pixels wide × 4 sub-pixels tall. World pixels from
     * geo_latlon_to_pixel already scale with 2^zoom — we must NOT scale
     * char_w with zoom too, or the two cancel and zooming stops working. */
    double char_w = 2.0;
    double char_h = 4.0;

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

    double char_w = 2.0;
    double char_h = 4.0;

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
    if (g_osm_initialized) {
        osm_free(&g_osm);
        g_osm_initialized = 0;
    }
    render_shutdown();
}

int map_tick(void)
{
    if (!g_osm_initialized) return 0;
    return osm_tick(&g_osm);
}

const char *map_status(void)
{
    if (!g_osm_initialized) return "";
    /* One-shot: reading the status also clears it so it doesn't stick in
     * the status bar forever after a tile finishes loading. */
    static char buf[128];
    strncpy(buf, g_osm.status, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    g_osm.status[0] = '\0';
    return buf;
}
