#ifndef ROUTE_H
#define ROUTE_H

#include <stddef.h>
#include "map.h"

#define MAX_STEPS 256

/* A single turn-by-turn step */
typedef struct {
    char   instruction[256];
    char   road_name[128];
    double distance_m;    /* meters */
    double duration_s;    /* seconds */
    double lat, lon;      /* maneuver location */
} RouteStep;

/* Full route result */
typedef struct {
    double     total_distance_m;
    double     total_duration_s;
    RouteStep  steps[MAX_STEPS];
    int        step_count;
    int        valid;
    char       error[256];
} RouteResult;

/* Query OSRM for a driving route between two points.
   Fills the RouteOverlay with the geometry and returns directions. */
RouteResult route_query(double orig_lat, double orig_lon,
                        double dest_lat, double dest_lon,
                        RouteOverlay *overlay);

/* Decode an encoded polyline string (Google format, precision 5)
   and add points to the overlay. */
int route_decode_polyline(const char *encoded, RouteOverlay *overlay);

/* Format duration in seconds to "Xh Ym" string */
void route_format_duration(double seconds, char *buf, size_t buf_sz);

/* Format distance in meters to "X.Y km" or "X.Y mi" string */
void route_format_distance(double meters, char *buf, size_t buf_sz, int use_miles);

#endif /* ROUTE_H */