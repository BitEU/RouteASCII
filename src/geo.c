#include "geo.h"
#include "http.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_RADIUS_KM 6371.0

/* --- Nominatim geocoding --- */

static void url_encode(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 3 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else {
            snprintf(dst + j, dst_sz - j, "%%%02X", c);
            j += 3;
        }
    }
    dst[j] = '\0';
}

GeoResult geo_search(const char *query)
{
    GeoResult result = {0};
    char encoded[512];
    char url[1024];

    url_encode(query, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
             "https://nominatim.openstreetmap.org/search?q=%s&format=json&limit=1",
             encoded);

    HttpBuffer buf = {0};
    if (http_get(url, &buf) != 0) return result;

    cJSON *root = cJSON_Parse(buf.data);
    http_buffer_free(&buf);
    if (!root) return result;

    if (cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
        cJSON *item = cJSON_GetArrayItem(root, 0);
        cJSON *lat_j = cJSON_GetObjectItem(item, "lat");
        cJSON *lon_j = cJSON_GetObjectItem(item, "lon");
        cJSON *name_j = cJSON_GetObjectItem(item, "display_name");

        if (lat_j && lon_j) {
            result.lat = atof(cJSON_GetStringValue(lat_j));
            result.lon = atof(cJSON_GetStringValue(lon_j));
            if (name_j && cJSON_GetStringValue(name_j)) {
                strncpy(result.display_name, cJSON_GetStringValue(name_j),
                        sizeof(result.display_name) - 1);
            }
            result.valid = 1;
        }
    }

    cJSON_Delete(root);
    return result;
}

GeoResult geo_reverse(double lat, double lon)
{
    GeoResult result = {0};
    char url[512];

    snprintf(url, sizeof(url),
             "https://nominatim.openstreetmap.org/reverse?lat=%.6f&lon=%.6f&format=json",
             lat, lon);

    HttpBuffer buf = {0};
    if (http_get(url, &buf) != 0) return result;

    cJSON *root = cJSON_Parse(buf.data);
    http_buffer_free(&buf);
    if (!root) return result;

    cJSON *name_j = cJSON_GetObjectItem(root, "display_name");
    if (name_j && cJSON_GetStringValue(name_j)) {
        result.lat = lat;
        result.lon = lon;
        strncpy(result.display_name, cJSON_GetStringValue(name_j),
                sizeof(result.display_name) - 1);
        result.valid = 1;
    }

    cJSON_Delete(root);
    return result;
}

/* --- Coordinate utilities --- */

void geo_latlon_to_tile(double lat, double lon, int zoom, int *tile_x, int *tile_y)
{
    int n = 1 << zoom;
    double lat_rad = lat * M_PI / 180.0;

    *tile_x = (int)((lon + 180.0) / 360.0 * n);
    *tile_y = (int)((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n);

    if (*tile_x < 0) *tile_x = 0;
    if (*tile_x >= n) *tile_x = n - 1;
    if (*tile_y < 0) *tile_y = 0;
    if (*tile_y >= n) *tile_y = n - 1;
}

void geo_tile_to_latlon(int tile_x, int tile_y, int zoom, double *lat, double *lon)
{
    int n = 1 << zoom;
    *lon = (double)tile_x / n * 360.0 - 180.0;
    double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * tile_y / n)));
    *lat = lat_rad * 180.0 / M_PI;
}

void geo_latlon_to_pixel(double lat, double lon, int zoom,
                         double *px_x, double *px_y)
{
    int n = 1 << zoom;
    double lat_rad = lat * M_PI / 180.0;

    *px_x = (lon + 180.0) / 360.0 * n * 256.0;
    *px_y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n * 256.0;
}

double geo_distance_km(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
               sin(dlon / 2) * sin(dlon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return EARTH_RADIUS_KM * c;
}