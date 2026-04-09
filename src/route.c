#include "route.h"
#include "http.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

int route_decode_polyline(const char *encoded, RouteOverlay *overlay)
{
    if (!encoded || !overlay) return -1;

    int index = 0;
    int len = (int)strlen(encoded);
    double lat = 0.0, lon = 0.0;
    int count = 0;

    while (index < len) {
        /* Decode latitude */
        int shift = 0, result = 0;
        int b;
        do {
            b = encoded[index++] - 63;
            result |= (b & 0x1f) << shift;
            shift += 5;
        } while (b >= 0x20 && index < len);
        double dlat = (result & 1) ? ~(result >> 1) : (result >> 1);
        lat += dlat / 1e5;

        /* Decode longitude */
        shift = 0;
        result = 0;
        do {
            b = encoded[index++] - 63;
            result |= (b & 0x1f) << shift;
            shift += 5;
        } while (b >= 0x20 && index < len);
        double dlng = (result & 1) ? ~(result >> 1) : (result >> 1);
        lon += dlng / 1e5;

        route_overlay_add_point(overlay, lat, lon);
        count++;
    }

    return count;
}

RouteResult route_query(double orig_lat, double orig_lon,
                        double dest_lat, double dest_lon,
                        RouteOverlay *overlay)
{
    RouteResult rr;
    memset(&rr, 0, sizeof(rr));

    char url[1024];
    snprintf(url, sizeof(url),
             "https://router.project-osrm.org/route/v1/driving/"
             "%.6f,%.6f;%.6f,%.6f"
             "?overview=full&geometries=polyline&steps=true",
             orig_lon, orig_lat, dest_lon, dest_lat);

    HttpBuffer buf = {0};
    if (http_get(url, &buf) != 0) {
        snprintf(rr.error, sizeof(rr.error), "HTTP request failed");
        return rr;
    }

    cJSON *root = cJSON_Parse(buf.data);
    http_buffer_free(&buf);

    if (!root) {
        snprintf(rr.error, sizeof(rr.error), "JSON parse failed");
        return rr;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || strcmp(cJSON_GetStringValue(code), "Ok") != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        snprintf(rr.error, sizeof(rr.error), "OSRM error: %s",
                 msg ? cJSON_GetStringValue(msg) : "unknown");
        cJSON_Delete(root);
        return rr;
    }

    cJSON *routes = cJSON_GetObjectItem(root, "routes");
    if (!cJSON_IsArray(routes) || cJSON_GetArraySize(routes) == 0) {
        snprintf(rr.error, sizeof(rr.error), "No routes found");
        cJSON_Delete(root);
        return rr;
    }

    cJSON *route = cJSON_GetArrayItem(routes, 0);

    /* Total distance and duration */
    cJSON *dist_j = cJSON_GetObjectItem(route, "distance");
    cJSON *dur_j  = cJSON_GetObjectItem(route, "duration");
    rr.total_distance_m = dist_j ? cJSON_GetNumberValue(dist_j) : 0;
    rr.total_duration_s = dur_j  ? cJSON_GetNumberValue(dur_j)  : 0;

    /* Decode geometry */
    cJSON *geom = cJSON_GetObjectItem(route, "geometry");
    if (geom && cJSON_IsString(geom)) {
        route_overlay_clear(overlay);
        route_decode_polyline(cJSON_GetStringValue(geom), overlay);
        overlay->orig_lat = orig_lat;
        overlay->orig_lon = orig_lon;
        overlay->dest_lat = dest_lat;
        overlay->dest_lon = dest_lon;
        overlay->has_route = 1;
    }

    /* Parse steps for turn-by-turn directions */
    cJSON *legs = cJSON_GetObjectItem(route, "legs");
    if (cJSON_IsArray(legs)) {
        int step_idx = 0;
        int nlegs = cJSON_GetArraySize(legs);
        for (int li = 0; li < nlegs && step_idx < MAX_STEPS; li++) {
            cJSON *leg = cJSON_GetArrayItem(legs, li);
            cJSON *steps = cJSON_GetObjectItem(leg, "steps");
            if (!cJSON_IsArray(steps)) continue;

            int nsteps = cJSON_GetArraySize(steps);
            for (int si = 0; si < nsteps && step_idx < MAX_STEPS; si++) {
                cJSON *step = cJSON_GetArrayItem(steps, si);
                RouteStep *rs = &rr.steps[step_idx];

                /* Maneuver */
                cJSON *maneuver = cJSON_GetObjectItem(step, "maneuver");
                if (maneuver) {
                    cJSON *mtype = cJSON_GetObjectItem(maneuver, "type");
                    cJSON *mmod  = cJSON_GetObjectItem(maneuver, "modifier");
                    cJSON *mloc  = cJSON_GetObjectItem(maneuver, "location");

                    const char *type_s = mtype ? cJSON_GetStringValue(mtype) : "";
                    const char *mod_s  = mmod  ? cJSON_GetStringValue(mmod)  : "";

                    snprintf(rs->instruction, sizeof(rs->instruction),
                             "%s %s", type_s, mod_s);

                    if (mloc && cJSON_IsArray(mloc) && cJSON_GetArraySize(mloc) >= 2) {
                        rs->lon = cJSON_GetNumberValue(cJSON_GetArrayItem(mloc, 0));
                        rs->lat = cJSON_GetNumberValue(cJSON_GetArrayItem(mloc, 1));
                    }
                }

                /* Road name */
                cJSON *name_j = cJSON_GetObjectItem(step, "name");
                if (name_j && cJSON_GetStringValue(name_j)) {
                    strncpy(rs->road_name, cJSON_GetStringValue(name_j),
                            sizeof(rs->road_name) - 1);
                }

                /* Distance & duration */
                cJSON *sd = cJSON_GetObjectItem(step, "distance");
                cJSON *st = cJSON_GetObjectItem(step, "duration");
                rs->distance_m = sd ? cJSON_GetNumberValue(sd) : 0;
                rs->duration_s = st ? cJSON_GetNumberValue(st) : 0;

                step_idx++;
            }
        }
        rr.step_count = step_idx;
    }

    rr.valid = 1;
    cJSON_Delete(root);
    return rr;
}

void route_format_duration(double seconds, char *buf, size_t buf_sz)
{
    int hrs = (int)(seconds / 3600);
    int mins = ((int)seconds % 3600) / 60;
    if (hrs > 0)
        snprintf(buf, buf_sz, "%dh %dm", hrs, mins);
    else
        snprintf(buf, buf_sz, "%dm", mins);
}

void route_format_distance(double meters, char *buf, size_t buf_sz, int use_miles)
{
    if (use_miles) {
        double mi = meters / 1609.344;
        if (mi < 0.1)
            snprintf(buf, buf_sz, "%d ft", (int)(meters * 3.28084));
        else
            snprintf(buf, buf_sz, "%.1f mi", mi);
    } else {
        if (meters < 1000)
            snprintf(buf, buf_sz, "%d m", (int)meters);
        else
            snprintf(buf, buf_sz, "%.1f km", meters / 1000.0);
    }
}