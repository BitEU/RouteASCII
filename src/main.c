#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "http.h"
#include "geo.h"
#include "map.h"
#include "route.h"
#include "ui.h"

static volatile int g_resize = 0;

#ifndef _WIN32
static void handle_winch(int sig)
{
    (void)sig;
    g_resize = 1;
}
#endif

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Initialize subsystems */
    http_init();

    UIState ui;
    ui_init(&ui);

    MapView mv;
    map_init(&mv);

    RouteOverlay overlay;
    route_overlay_init(&overlay);

    RouteResult route_result;
    memset(&route_result, 0, sizeof(route_result));

    char status_msg[256] = "";

#ifndef _WIN32
    signal(SIGWINCH, handle_winch);
#endif

    int running = 1;
    while (running) {
        /* Handle terminal resize */
        if (g_resize) {
            g_resize = 0;
            endwin();
            refresh();
            ui_resize(&ui);
        }

        /* Render */
        map_render(ui.map_win, &mv, &overlay);
        ui_draw_status(&ui, &mv, status_msg);
        if (ui.sidebar_visible) {
            ui_draw_sidebar(&ui, &route_result);
        }

        /* Handle input */
        int ch = getch();
        status_msg[0] = '\0';

        switch (ch) {
        case 'q':
        case 'Q':
        case 27: /* ESC */
            running = 0;
            break;

        case KEY_UP:
        case 'k':
            map_pan(&mv, 0, -1);
            break;
        case KEY_DOWN:
        case 'j':
            map_pan(&mv, 0, 1);
            break;
        case KEY_LEFT:
        case 'h':
            map_pan(&mv, -1, 0);
            break;
        case KEY_RIGHT:
        case 'l':
            map_pan(&mv, 1, 0);
            break;

        case '+':
        case '=':
            map_zoom_in(&mv);
            snprintf(status_msg, sizeof(status_msg), "Zoom: %d", mv.zoom);
            break;
        case '-':
        case '_':
            map_zoom_out(&mv);
            snprintf(status_msg, sizeof(status_msg), "Zoom: %d", mv.zoom);
            break;

        case 'g':
        case 'G': {
            /* Goto: geocode a place name */
            char query[256];
            if (ui_input_dialog(&ui, "Go to (place name)", query, sizeof(query))) {
                snprintf(status_msg, sizeof(status_msg), "Searching: %s...", query);
                ui_draw_status(&ui, &mv, status_msg);
                doupdate();

                GeoResult gr = geo_search(query);
                if (gr.valid) {
                    map_center_on(&mv, gr.lat, gr.lon);
                    snprintf(status_msg, sizeof(status_msg),
                             "Found: %.60s", gr.display_name);
                } else {
                    snprintf(status_msg, sizeof(status_msg),
                             "Not found: %s", query);
                }
            }
            break;
        }

        case 'r':
        case 'R': {
            /* Route: enter origin and destination */
            char orig_str[256], dest_str[256];

            if (!ui_input_dialog(&ui, "Origin (place name or lat,lon)",
                                 orig_str, sizeof(orig_str)))
                break;
            if (!ui_input_dialog(&ui, "Destination (place name or lat,lon)",
                                 dest_str, sizeof(dest_str)))
                break;

            snprintf(status_msg, sizeof(status_msg), "Calculating route...");
            ui_draw_status(&ui, &mv, status_msg);
            doupdate();

            /* Geocode origin */
            GeoResult orig_geo = geo_search(orig_str);
            if (!orig_geo.valid) {
                snprintf(status_msg, sizeof(status_msg),
                         "Could not find origin: %s", orig_str);
                break;
            }

            /* Geocode destination */
            GeoResult dest_geo = geo_search(dest_str);
            if (!dest_geo.valid) {
                snprintf(status_msg, sizeof(status_msg),
                         "Could not find destination: %s", dest_str);
                break;
            }

            /* Query OSRM */
            route_result = route_query(orig_geo.lat, orig_geo.lon,
                                       dest_geo.lat, dest_geo.lon,
                                       &overlay);

            if (route_result.valid) {
                /* Auto-zoom to show the route */
                double mid_lat = (orig_geo.lat + dest_geo.lat) / 2.0;
                double mid_lon = (orig_geo.lon + dest_geo.lon) / 2.0;
                map_center_on(&mv, mid_lat, mid_lon);

                /* Estimate good zoom level */
                double dist = geo_distance_km(orig_geo.lat, orig_geo.lon,
                                              dest_geo.lat, dest_geo.lon);
                if (dist > 5000)      mv.zoom = 3;
                else if (dist > 2000) mv.zoom = 4;
                else if (dist > 1000) mv.zoom = 5;
                else if (dist > 500)  mv.zoom = 6;
                else if (dist > 200)  mv.zoom = 7;
                else if (dist > 100)  mv.zoom = 8;
                else if (dist > 50)   mv.zoom = 9;
                else if (dist > 20)   mv.zoom = 10;
                else                  mv.zoom = 11;

                char dist_buf[32], dur_buf[32];
                route_format_distance(route_result.total_distance_m,
                                      dist_buf, sizeof(dist_buf), 1);
                route_format_duration(route_result.total_duration_s,
                                      dur_buf, sizeof(dur_buf));

                snprintf(status_msg, sizeof(status_msg),
                         "Route: %s, %s (%d steps). Press [d] for directions.",
                         dist_buf, dur_buf, route_result.step_count);

                /* Auto-show sidebar */
                if (!ui.sidebar_visible) {
                    ui_toggle_sidebar(&ui);
                }
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "Route error: %s", route_result.error);
            }
            break;
        }

        case 'd':
        case 'D':
            ui_toggle_sidebar(&ui);
            break;

        case 'c':
        case 'C':
            route_overlay_clear(&overlay);
            memset(&route_result, 0, sizeof(route_result));
            snprintf(status_msg, sizeof(status_msg), "Route cleared.");
            if (ui.sidebar_visible) ui_toggle_sidebar(&ui);
            break;

        /* Sidebar scrolling */
        case KEY_PPAGE:
            if (ui.sidebar_visible && ui.sidebar_scroll > 0)
                ui.sidebar_scroll -= 5;
            if (ui.sidebar_scroll < 0) ui.sidebar_scroll = 0;
            break;
        case KEY_NPAGE:
            if (ui.sidebar_visible)
                ui.sidebar_scroll += 5;
            break;

#ifdef KEY_RESIZE
        case KEY_RESIZE:
            endwin();
            refresh();
            ui_resize(&ui);
            break;
#endif

        case ERR:
            /* Timeout — no key pressed, just re-render */
            break;

        default:
            break;
        }
    }

    /* Cleanup */
    route_overlay_free(&overlay);
    ui_cleanup(&ui);
    http_cleanup();

    return 0;
}