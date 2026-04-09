#define _MAP_NEEDS_CURSES
#include "map.h"
#include "geo.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MIN_ZOOM 2
#define MAX_ZOOM 16

/* ASCII shading ramp — used to give visual weight to landmass vs water.
   Since we don't have actual tile image data in a pure-ASCII approach,
   we render a simplified world outline + coordinate grid. */

/* Simplified continent boundary test using a very coarse bitmask approach.
   For a real app you'd decode PNG tiles — here we use an embedded low-res
   world bitmap for offline rendering. */

/* 72x36 coarse world land mask (each cell = 5° × 5°)
   1 = land, 0 = water. Row 0 = 90°N, Col 0 = 180°W */
static const unsigned char WORLD_LAND[36][72] = {
    /* 90N */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 85N */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 80N */ {0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0},
    /* 75N */ {0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,0,0},
    /* 70N */ {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0},
    /* 65N */ {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0},
    /* 60N */ {0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0},
    /* 55N */ {0,0,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0},
    /* 50N */ {0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0},
    /* 45N */ {0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
    /* 40N */ {0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0},
    /* 35N */ {0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0},
    /* 30N */ {0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0},
    /* 25N */ {0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
    /* 20N */ {0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0},
    /* 15N */ {0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
    /* 10N */ {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 5N  */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* EQ  */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 5S  */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
    /* 10S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0},
    /* 15S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0},
    /* 20S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0},
    /* 25S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0},
    /* 30S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
    /* 35S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 40S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 45S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0},
    /* 50S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 55S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 60S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 65S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 70S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 75S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 80S */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* Characters for rendering */
#define CH_WATER    '~'
#define CH_LAND     '#'
#define CH_LAND2    '%'
#define CH_BORDER   '.'
#define CH_ROUTE    '*'
#define CH_ORIGIN   'A'
#define CH_DEST     'B'
#define CH_GRID     '+'

/* Check if a lat/lon is over land using the coarse bitmap */
static int is_land(double lat, double lon)
{
    /* Map lat (-90..90) to row (35..0), lon (-180..180) to col (0..71) */
    int row = (int)((90.0 - lat) / 5.0);
    int col = (int)((lon + 180.0) / 5.0);
    if (row < 0) row = 0;
    if (row > 35) row = 35;
    if (col < 0) col = 0;
    if (col > 71) col = 71;
    return WORLD_LAND[row][col];
}

void map_init(MapView *mv)
{
    /* Default: centered on the USA */
    mv->center_lat = 39.8;
    mv->center_lon = -98.5;
    mv->zoom = 4;
    mv->screen_w = 80;
    mv->screen_h = 24;
}

/* Compute the lat/lon for a screen cell */
static void screen_cell_to_latlon(MapView *mv, int col, int row,
                                  double *lat, double *lon)
{
    /* Each character cell represents a certain geographic span.
       Terminal chars are ~2:1 aspect ratio (taller than wide),
       so we scale accordingly. */
    double center_px, center_py;
    geo_latlon_to_pixel(mv->center_lat, mv->center_lon, mv->zoom,
                        &center_px, &center_py);

    /* Pixels per character cell — adjust for terminal aspect ratio */
    double scale = 256.0 / (1 << (18 - mv->zoom));  /* pixels per char approx */
    if (scale < 1.0) scale = 1.0;

    /* Terminal chars are roughly 2x taller than wide */
    double char_w = scale * 2.0;
    double char_h = scale * 4.0;

    double px = center_px + (col - mv->screen_w / 2.0) * char_w;
    double py = center_py + (row - mv->screen_h / 2.0) * char_h;

    /* Convert back to lat/lon */
    int n = 1 << mv->zoom;
    *lon = px / (n * 256.0) * 360.0 - 180.0;
    double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * py / (n * 256.0))));
    *lat = lat_rad * 180.0 / M_PI;
}

void map_render(WINDOW *win, MapView *mv, RouteOverlay *overlay)
{
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    mv->screen_w = max_x;
    mv->screen_h = max_y;

    /* First pass: render base map */
    for (int row = 0; row < max_y; row++) {
        for (int col = 0; col < max_x; col++) {
            double lat, lon;
            screen_cell_to_latlon(mv, col, row, &lat, &lon);

            /* Clamp */
            if (lat > 85.0) lat = 85.0;
            if (lat < -85.0) lat = -85.0;
            while (lon > 180.0) lon -= 360.0;
            while (lon < -180.0) lon += 360.0;

            int land = is_land(lat, lon);

            /* Grid lines every 10 degrees */
            int on_grid = 0;
            double glat = fmod(fabs(lat), 10.0);
            double glon = fmod(fabs(lon), 10.0);
            if (glat < 0.5 || glon < 0.5) on_grid = 1;

            chtype ch;
            if (land) {
                if (on_grid)
                    ch = CH_BORDER | COLOR_PAIR(3);
                else
                    ch = CH_LAND | COLOR_PAIR(2);
            } else {
                if (on_grid)
                    ch = CH_GRID | COLOR_PAIR(4);
                else
                    ch = CH_WATER | COLOR_PAIR(1);
            }

            mvwaddch(win, row, col, ch);
        }
    }

    /* Second pass: render route overlay */
    if (overlay && overlay->has_route && overlay->count > 0) {
        for (int i = 0; i < overlay->count; i++) {
            int sc, sr;
            if (map_latlon_to_screen(mv, overlay->points[i].lat,
                                     overlay->points[i].lon, &sc, &sr)) {
                mvwaddch(win, sr, sc, CH_ROUTE | COLOR_PAIR(5) | A_BOLD);
            }
        }

        /* Draw origin marker */
        {
            int sc, sr;
            if (map_latlon_to_screen(mv, overlay->orig_lat,
                                     overlay->orig_lon, &sc, &sr)) {
                mvwaddch(win, sr, sc, CH_ORIGIN | COLOR_PAIR(6) | A_BOLD);
            }
        }
        /* Draw destination marker */
        {
            int sc, sr;
            if (map_latlon_to_screen(mv, overlay->dest_lat,
                                     overlay->dest_lon, &sc, &sr)) {
                mvwaddch(win, sr, sc, CH_DEST | COLOR_PAIR(6) | A_BOLD);
            }
        }
    }

    wrefresh(win);
}

void map_pan(MapView *mv, int dx, int dy)
{
    /* Convert dx/dy character cells to lat/lon shift */
    double lat1, lon1, lat2, lon2;
    screen_cell_to_latlon(mv, mv->screen_w / 2, mv->screen_h / 2, &lat1, &lon1);
    screen_cell_to_latlon(mv, mv->screen_w / 2 + 1, mv->screen_h / 2 + 1, &lat2, &lon2);

    double dlon = (lon2 - lon1) * dx * 3;
    double dlat = (lat2 - lat1) * dy * 3;

    mv->center_lon += dlon;
    mv->center_lat -= dlat;

    /* Clamp */
    if (mv->center_lat > 85.0) mv->center_lat = 85.0;
    if (mv->center_lat < -85.0) mv->center_lat = -85.0;
    while (mv->center_lon > 180.0) mv->center_lon -= 360.0;
    while (mv->center_lon < -180.0) mv->center_lon += 360.0;
}

int map_zoom_in(MapView *mv)
{
    if (mv->zoom < MAX_ZOOM) mv->zoom++;
    return mv->zoom;
}

int map_zoom_out(MapView *mv)
{
    if (mv->zoom > MIN_ZOOM) mv->zoom--;
    return mv->zoom;
}

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

    double scale = 256.0 / (1 << (18 - mv->zoom));
    if (scale < 1.0) scale = 1.0;
    double char_w = scale * 2.0;
    double char_h = scale * 4.0;

    *col = (int)(mv->screen_w / 2.0 + (pt_px - center_px) / char_w);
    *row = (int)(mv->screen_h / 2.0 + (pt_py - center_py) / char_h);

    return (*col >= 0 && *col < mv->screen_w && *row >= 0 && *row < mv->screen_h);
}

/* Route overlay */

void route_overlay_init(RouteOverlay *ro)
{
    memset(ro, 0, sizeof(*ro));
}

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