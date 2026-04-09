#ifndef GEO_H
#define GEO_H

/* A geocoded location */
typedef struct {
    double lat;
    double lon;
    char   display_name[256];
    int    valid;
} GeoResult;

/* Geocode a place name string. Returns result with valid=1 on success. */
GeoResult geo_search(const char *query);

/* Reverse geocode lat/lon to a place name. */
GeoResult geo_reverse(double lat, double lon);

/* --- Coordinate utilities --- */

/* Convert lat/lon to Mercator tile coordinates at given zoom */
void geo_latlon_to_tile(double lat, double lon, int zoom, int *tile_x, int *tile_y);

/* Convert tile coordinates back to lat/lon (top-left corner of tile) */
void geo_tile_to_latlon(int tile_x, int tile_y, int zoom, double *lat, double *lon);

/* Convert lat/lon to pixel coordinates within a tile at given zoom */
void geo_latlon_to_pixel(double lat, double lon, int zoom,
                         double *px_x, double *px_y);

/* Haversine distance in km between two lat/lon points */
double geo_distance_km(double lat1, double lon1, double lat2, double lon2);

#endif /* GEO_H */