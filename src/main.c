#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

#define _MAP_NEEDS_CURSES
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

static int parse_latlon_input(const char *text, double *lat, double *lon)
{
    if (!text || !lat || !lon) return 0;

    const char *p = text;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) return 0;

    char *end = NULL;
    double a = strtod(p, &end);
    if (end == p) return 0;
    p = end;

    while (isspace((unsigned char)*p)) p++;
    if (*p == ',' || *p == ';') p++;
    while (isspace((unsigned char)*p)) p++;

    double b = strtod(p, &end);
    if (end == p) return 0;
    p = end;

    while (isspace((unsigned char)*p)) p++;
    if (*p != '\0') return 0;

    if (a < -90.0 || a > 90.0 || b < -180.0 || b > 180.0) return 0;

    *lat = a;
    *lon = b;
    return 1;
}

static GeoResult resolve_location_input(const char *input, const char *label)
{
    GeoResult gr = {0};
    double lat = 0.0, lon = 0.0;

    if (parse_latlon_input(input, &lat, &lon)) {
        gr.valid = 1;
        gr.lat = lat;
        gr.lon = lon;
        snprintf(gr.display_name, sizeof(gr.display_name),
                 "%.6f, %.6f", lat, lon);
        fprintf(stderr, "[geo] %s parsed as coordinates: %.6f, %.6f\n",
                label ? label : "location", lat, lon);
        return gr;
    }

    fprintf(stderr, "[geo] %s geocoding query: \"%s\"\n",
            label ? label : "location", input ? input : "");
    gr = geo_search(input);
    if (gr.valid) {
        fprintf(stderr, "[geo] %s resolved to %.6f, %.6f (%s)\n",
                label ? label : "location", gr.lat, gr.lon, gr.display_name);
    } else {
        fprintf(stderr, "[geo] %s geocoding failed for \"%s\"\n",
                label ? label : "location", input ? input : "");
    }
    return gr;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Redirect stderr to console.log before curses takes the terminal.
     * Without this, all the [geodata]/[osm] fprintf lines scribble over
     * the TUI and then get wiped on the next redraw. Line-buffer so tail
     * -f works and so crashes flush everything written so far. */
    FILE *logf = freopen("console.log", "w", stderr);
    if (logf) setvbuf(logf, NULL, _IONBF, 0);

        fprintf(stderr, "[app] starting RouteASCII\n");

    /* Initialize subsystems */
    http_init();
        fprintf(stderr, "[app] HTTP subsystem initialized\n");

    UIState ui;
    ui_init(&ui);
        fprintf(stderr, "[app] UI initialized (%dx%d)\n", ui.term_w, ui.term_h);

    MapView mv;
    map_init(&mv);
        fprintf(stderr, "[app] map initialized at lat=%.4f lon=%.4f zoom=%d\n",
            mv.center_lat, mv.center_lon, mv.zoom);

    RouteOverlay overlay;
    route_overlay_init(&overlay);
        fprintf(stderr, "[app] route overlay initialized\n");

    RouteResult route_result;
    memset(&route_result, 0, sizeof(route_result));
    char route_origin_label[256] = "";
    char route_dest_label[256] = "";

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
            map_pan(&mv, 0, 1);
            break;
        case KEY_DOWN:
        case 'j':
            map_pan(&mv, 0, -1);
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

                GeoResult gr = resolve_location_input(query, "goto");
                if (gr.valid) {
                    map_center_on(&mv, gr.lat, gr.lon);
                    snprintf(status_msg, sizeof(status_msg),
                             "Found: %.60s", gr.display_name);
                    fprintf(stderr, "[app] goto success -> center %.6f, %.6f\n",
                            gr.lat, gr.lon);
                } else {
                    snprintf(status_msg, sizeof(status_msg),
                             "Not found: %s", query);
                    fprintf(stderr, "[app] goto failed for \"%s\"\n", query);
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
            fprintf(stderr, "[app] route requested: origin=\"%s\" destination=\"%s\"\n",
                    orig_str, dest_str);

            /* Geocode origin */
            GeoResult orig_geo = resolve_location_input(orig_str, "origin");
            if (!orig_geo.valid) {
                snprintf(status_msg, sizeof(status_msg),
                         "Could not find origin: %s", orig_str);
                fprintf(stderr, "[app] route aborted: origin lookup failed\n");
                break;
            }

            /* Geocode destination */
            GeoResult dest_geo = resolve_location_input(dest_str, "destination");
            if (!dest_geo.valid) {
                snprintf(status_msg, sizeof(status_msg),
                         "Could not find destination: %s", dest_str);
                fprintf(stderr, "[app] route aborted: destination lookup failed\n");
                break;
            }

            /* Query OSRM */
            fprintf(stderr, "[app] querying route %.6f,%.6f -> %.6f,%.6f\n",
                    orig_geo.lat, orig_geo.lon, dest_geo.lat, dest_geo.lon);
            route_result = route_query(orig_geo.lat, orig_geo.lon,
                                       dest_geo.lat, dest_geo.lon,
                                       &overlay);

            if (route_result.valid) {
                const char *resolved_orig = orig_geo.display_name[0]
                    ? orig_geo.display_name : orig_str;
                const char *resolved_dest = dest_geo.display_name[0]
                    ? dest_geo.display_name : dest_str;
                strncpy(route_origin_label, resolved_orig,
                        sizeof(route_origin_label) - 1);
                route_origin_label[sizeof(route_origin_label) - 1] = '\0';
                strncpy(route_dest_label, resolved_dest,
                        sizeof(route_dest_label) - 1);
                route_dest_label[sizeof(route_dest_label) - 1] = '\0';

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
                fprintf(stderr, "[app] route success: %s, %s, %d steps\n",
                        dist_buf, dur_buf, route_result.step_count);

                /* Auto-show sidebar */
                if (!ui.sidebar_visible) {
                    ui_toggle_sidebar(&ui);
                }
            } else {
                route_origin_label[0] = '\0';
                route_dest_label[0] = '\0';
                snprintf(status_msg, sizeof(status_msg),
                         "Route error: %s", route_result.error);
                fprintf(stderr, "[app] route error: %s\n", route_result.error);
            }
            break;
        }

        case 'd':
        case 'D':
            ui_toggle_sidebar(&ui);
            break;

        case 'p':
        case 'P': {
            if (!route_result.valid || route_result.step_count <= 0) {
                snprintf(status_msg, sizeof(status_msg),
                         "No directions to print. Press [r] to route first.");
                break;
            }

            char out_path[512];
            char err_buf[256];
            if (route_export_directions(&route_result,
                                        route_origin_label,
                                        route_dest_label,
                                        out_path, sizeof(out_path),
                                        err_buf, sizeof(err_buf)) == 0) {
                snprintf(status_msg, sizeof(status_msg),
                         "Directions saved: %s", out_path);
                fprintf(stderr, "[app] directions exported to %s\n", out_path);
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "Print failed: %s", err_buf);
                fprintf(stderr, "[app] directions export failed: %s\n", err_buf);
            }
            break;
        }

        case 'c':
        case 'C':
            route_overlay_clear(&overlay);
            memset(&route_result, 0, sizeof(route_result));
            route_origin_label[0] = '\0';
            route_dest_label[0] = '\0';
            snprintf(status_msg, sizeof(status_msg), "Route cleared.");
            fprintf(stderr, "[app] route cleared by user\n");
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
            /* Timeout — drive background work (e.g. OSM tile fetches).
             * If something changed, surface the status in the status bar
             * so the user sees what's happening. */
            map_tick();
            {
                const char *ms = map_status();
                if (ms && *ms) {
                    strncpy(status_msg, ms, sizeof(status_msg) - 1);
                    status_msg[sizeof(status_msg) - 1] = '\0';
                }
            }
            break;

        default:
            break;
        }
    }

    /* Cleanup */
    route_overlay_free(&overlay);
    map_shutdown();
    ui_cleanup(&ui);
    http_cleanup();
    fprintf(stderr, "[app] shutdown complete\n");

    return 0;
}