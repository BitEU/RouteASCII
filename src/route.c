#include "route.h"
#include "http.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define OSRM_DEFAULT_URL "http://127.0.0.1:5000"

static const char *env_nonempty(const char *name)
{
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static long env_long_or_default(const char *name, long def,
                                long min_value, long max_value)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return def;

    char *end = NULL;
    long parsed = strtol(v, &end, 10);
    if (end == v || (end && *end != '\0')) return def;
    if (parsed < min_value) return min_value;
    if (parsed > max_value) return max_value;
    return parsed;
}

static void trim_trailing_slashes(const char *src, char *dst, size_t dst_sz)
{
    if (!src || !dst || dst_sz == 0) return;
    size_t n = strlen(src);
    while (n > 0 && src[n - 1] == '/') n--;
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int same_base_url(const char *a, const char *b)
{
    if (!a || !b) return 0;
    char na[512], nb[512];
    trim_trailing_slashes(a, na, sizeof(na));
    trim_trailing_slashes(b, nb, sizeof(nb));
    return strcmp(na, nb) == 0;
}

static void build_route_url(char *dst, size_t dst_sz, const char *base,
                            double orig_lat, double orig_lon,
                            double dest_lat, double dest_lon)
{
    char normalized_base[512];
    trim_trailing_slashes(base, normalized_base, sizeof(normalized_base));
    snprintf(dst, dst_sz,
             "%s/route/v1/driving/"
             "%.6f,%.6f;%.6f,%.6f"
             "?overview=full&geometries=polyline&steps=true",
             normalized_base,
             orig_lon, orig_lat, dest_lon, dest_lat);
}

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

    const char *primary_base = env_nonempty("ROUTEASCII_OSRM_URL");
    const char *fallback_base = env_nonempty("ROUTEASCII_OSRM_FALLBACK_URL");
    if (!primary_base) primary_base = OSRM_DEFAULT_URL;

    long primary_timeout_s = env_long_or_default("ROUTEASCII_OSRM_TIMEOUT_S", 25, 1, 600);
    long primary_connect_timeout_s = env_long_or_default("ROUTEASCII_OSRM_CONNECT_TIMEOUT_S", 3, 1, 60);
    long fallback_timeout_s = env_long_or_default("ROUTEASCII_OSRM_FALLBACK_TIMEOUT_S", 120, 1, 600);
    long fallback_connect_timeout_s = env_long_or_default("ROUTEASCII_OSRM_FALLBACK_CONNECT_TIMEOUT_S", 10, 1, 60);

    char url[1024];
    build_route_url(url, sizeof(url), primary_base,
                    orig_lat, orig_lon, dest_lat, dest_lon);

    fprintf(stderr,
            "[route] OSRM request origin=(%.6f,%.6f) dest=(%.6f,%.6f)\n",
            orig_lat, orig_lon, dest_lat, dest_lon);
    fprintf(stderr, "[route] endpoint: %s\n", primary_base);
    fprintf(stderr, "[route] URL: %s\n", url);

    HttpBuffer buf = {0};
    if (http_get_timeout(url, &buf, primary_timeout_s, primary_connect_timeout_s) != 0) {
        const char *primary_err = http_last_error();
        char primary_err_copy[256] = "unknown error";
        if (primary_err && primary_err[0]) {
            strncpy(primary_err_copy, primary_err, sizeof(primary_err_copy) - 1);
            primary_err_copy[sizeof(primary_err_copy) - 1] = '\0';
        }
        fprintf(stderr, "[route] primary endpoint failed: %s\n", primary_err_copy);

        if (fallback_base && fallback_base[0] && !same_base_url(primary_base, fallback_base)) {
            fprintf(stderr, "[route] trying fallback endpoint: %s\n", fallback_base);
            build_route_url(url, sizeof(url), fallback_base,
                            orig_lat, orig_lon, dest_lat, dest_lon);
            fprintf(stderr, "[route] fallback URL: %s\n", url);
            if (http_get_timeout(url, &buf, fallback_timeout_s, fallback_connect_timeout_s) != 0) {
                const char *fallback_err = http_last_error();
                snprintf(rr.error, sizeof(rr.error),
                         "OSRM failed (primary: %s, fallback: %s)",
                         primary_err_copy,
                         (fallback_err && fallback_err[0]) ? fallback_err : "unknown error");
                fprintf(stderr, "[route] %s\n", rr.error);
                return rr;
            }
            fprintf(stderr, "[route] fallback endpoint succeeded\n");
        } else {
            if (same_base_url(primary_base, OSRM_DEFAULT_URL)) {
                snprintf(rr.error, sizeof(rr.error),
                         "Local OSRM unavailable at %s (%s)",
                         primary_base, primary_err_copy);
            } else {
                snprintf(rr.error, sizeof(rr.error),
                         "OSRM request failed at %s (%s)",
                         primary_base, primary_err_copy);
            }
            fprintf(stderr, "[route] %s\n", rr.error);
            return rr;
        }
    }

    fprintf(stderr, "[route] OSRM response size: %zu bytes\n", buf.size);

    cJSON *root = cJSON_Parse(buf.data);
    if (!root) {
        fprintf(stderr, "[route] JSON parse failed. body preview: %.120s\n",
                buf.data ? buf.data : "");
        http_buffer_free(&buf);
        snprintf(rr.error, sizeof(rr.error), "JSON parse failed");
        return rr;
    }
    http_buffer_free(&buf);

    cJSON *code = cJSON_GetObjectItem(root, "code");
    const char *code_s = (code && cJSON_IsString(code)) ? cJSON_GetStringValue(code) : NULL;
    if (!code_s || strcmp(code_s, "Ok") != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        const char *msg_s = (msg && cJSON_IsString(msg)) ? cJSON_GetStringValue(msg) : "unknown";
        snprintf(rr.error, sizeof(rr.error), "OSRM error: %s",
                 msg_s ? msg_s : "unknown");
        fprintf(stderr, "[route] OSRM returned code=%s message=%s\n",
                code_s ? code_s : "(missing)", msg_s ? msg_s : "unknown");
        cJSON_Delete(root);
        return rr;
    }

    cJSON *routes = cJSON_GetObjectItem(root, "routes");
    if (!cJSON_IsArray(routes) || cJSON_GetArraySize(routes) == 0) {
        snprintf(rr.error, sizeof(rr.error), "No routes found");
        fprintf(stderr, "[route] OSRM returned no routes\n");
        cJSON_Delete(root);
        return rr;
    }

    cJSON *route = cJSON_GetArrayItem(routes, 0);

    /* Total distance and duration */
    cJSON *dist_j = cJSON_GetObjectItem(route, "distance");
    cJSON *dur_j  = cJSON_GetObjectItem(route, "duration");
    rr.total_distance_m = dist_j ? cJSON_GetNumberValue(dist_j) : 0;
    rr.total_duration_s = dur_j  ? cJSON_GetNumberValue(dur_j)  : 0;
    fprintf(stderr, "[route] summary: distance=%.1fm duration=%.1fs\n",
            rr.total_distance_m, rr.total_duration_s);

    /* Decode geometry */
    cJSON *geom = cJSON_GetObjectItem(route, "geometry");
    if (geom && cJSON_IsString(geom)) {
        route_overlay_clear(overlay);
        int decoded = route_decode_polyline(cJSON_GetStringValue(geom), overlay);
        overlay->orig_lat = orig_lat;
        overlay->orig_lon = orig_lon;
        overlay->dest_lat = dest_lat;
        overlay->dest_lon = dest_lon;
        overlay->has_route = 1;
        fprintf(stderr, "[route] decoded %d polyline points\n", decoded);
    } else {
        fprintf(stderr, "[route] warning: route geometry missing or not a string\n");
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
        fprintf(stderr, "[route] parsed %d turn steps\n", rr.step_count);
    }

    rr.valid = 1;
    fprintf(stderr, "[route] route query complete\n");
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