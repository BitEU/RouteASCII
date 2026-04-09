#include "render.h"
#include "geo.h"
#include "ui.h"
#include "subpixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Module-owned braille canvas for all stroke primitives. */
static BrailleCanvas g_bc;

void render_begin_frame(WINDOW *win)
{
    int mx, my;
    getmaxyx(win, my, mx);
    bc_resize(&g_bc, mx, my);
}

void render_end_frame(WINDOW *win)
{
    bc_flush(&g_bc, win);
}

void render_shutdown(void)
{
    bc_free(&g_bc);
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * Projection helpers. MapView stores a center (lat,lon) + zoom; we want
 * cell-space (col,row) coords for any (lat,lon) consistently with map.c.
 *
 * One character cell = 2 sub-pixels wide × 4 sub-pixels tall (braille).
 * World-pixel coordinates from geo_latlon_to_pixel() already scale with
 * 2^zoom, so char_w / char_h are simply constants — they define how many
 * Mercator "world pixels" fit into one screen cell, not how we scale zoom.
 *
 * (The previous code multiplied char_w by 2^(zoom-9), which perfectly
 * cancelled the zoom growth in the numerator — every zoom level above 10
 * rendered identically. That's why zoom 10 and zoom 16 looked the same.)
 * -------------------------------------------------------------------------*/

#define CELL_SUBPX_W 2.0
#define CELL_SUBPX_H 4.0

typedef struct {
    double center_px, center_py;
    double char_w, char_h;
    int    sw, sh;
    double lon_offset;   /* 0, +360, or -360 for antimeridian passes */
} Projector;

static void projector_init(Projector *p, MapView *mv, double lon_offset)
{
    geo_latlon_to_pixel(mv->center_lat, mv->center_lon, mv->zoom,
                        &p->center_px, &p->center_py);
    p->char_w = CELL_SUBPX_W;
    p->char_h = CELL_SUBPX_H;
    p->sw = mv->screen_w;
    p->sh = mv->screen_h;
    p->lon_offset = lon_offset;
}

/* Project lat/lon to fractional cell coords at the view's zoom level. */
static inline void project_at(MapView *mv, const Projector *p,
                              double lat, double lon,
                              double *cx, double *cy)
{
    double px, py;
    geo_latlon_to_pixel(lat, lon + p->lon_offset, mv->zoom, &px, &py);
    *cx = p->sw / 2.0 + (px - p->center_px) / p->char_w;
    *cy = p->sh / 2.0 + (py - p->center_py) / p->char_h;
}

/* Compute viewport bbox in lat/lon (for a given lon_offset pass). */
static void viewport_bbox(MapView *mv, double lon_offset, GeoBBox *out)
{
    /* Sample the four screen corners back to lat/lon. The top/bottom rows
     * give lat extents; left/right cols give lon extents. */
    double lat_tl, lon_tl, lat_br, lon_br;
    /* Use the inverse of project_at: start from cell coords 0,0 and sw-1,sh-1 */
    double center_px, center_py;
    geo_latlon_to_pixel(mv->center_lat, mv->center_lon, mv->zoom,
                        &center_px, &center_py);
    double char_w = CELL_SUBPX_W, char_h = CELL_SUBPX_H;

    double px_l = center_px + (0            - mv->screen_w / 2.0) * char_w;
    double px_r = center_px + (mv->screen_w - mv->screen_w / 2.0) * char_w;
    double py_t = center_py + (0            - mv->screen_h / 2.0) * char_h;
    double py_b = center_py + (mv->screen_h - mv->screen_h / 2.0) * char_h;

    int n = 1 << mv->zoom;
    lon_tl = px_l / (n * 256.0) * 360.0 - 180.0;
    lon_br = px_r / (n * 256.0) * 360.0 - 180.0;
    lat_tl = atan(sinh(M_PI * (1.0 - 2.0 * py_t / (n * 256.0)))) * 180.0 / M_PI;
    lat_br = atan(sinh(M_PI * (1.0 - 2.0 * py_b / (n * 256.0)))) * 180.0 / M_PI;

    out->min_lon = (float)(lon_tl - lon_offset);
    out->max_lon = (float)(lon_br - lon_offset);
    out->min_lat = (float)(lat_br < lat_tl ? lat_br : lat_tl);
    out->max_lat = (float)(lat_br > lat_tl ? lat_br : lat_tl);
    /* Add small margin so features don't pop at edges */
    float lon_m = (out->max_lon - out->min_lon) * 0.05f;
    float lat_m = (out->max_lat - out->min_lat) * 0.05f;
    out->min_lon -= lon_m; out->max_lon += lon_m;
    out->min_lat -= lat_m; out->max_lat += lat_m;
}

static int bbox_intersects(const GeoBBox *a, const GeoBBox *b)
{
    if (a->max_lon < b->min_lon || a->min_lon > b->max_lon) return 0;
    if (a->max_lat < b->min_lat || a->min_lat > b->max_lat) return 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Cell put — bounds-checked character write with color.
 * -------------------------------------------------------------------------*/
static inline void put_cell(WINDOW *win, int c, int r, int ch, int cp, int attr)
{
    int mx, my;
    getmaxyx(win, my, mx);
    if (c < 0 || r < 0 || c >= mx || r >= my) return;
    /* leave the bottom row alone if the window has one — some curses impls
     * misbehave when writing the last cell. We already don't get the last
     * col when using mvwaddch typically. We'll accept that risk for now. */
    if (c == mx - 1 && r == my - 1) return;
    mvwaddch(win, r, c, (chtype)ch | COLOR_PAIR(cp) | attr);
}

/* ---------------------------------------------------------------------------
 * Cohen-Sutherland line clipping against the window rect. Returns 1 if the
 * segment is at least partially visible (and updates endpoints in place),
 * 0 if it's fully outside. Keeps Bresenham from looping over millions of
 * off-screen cells for far-away feature edges.
 * -------------------------------------------------------------------------*/
#define CS_INSIDE 0
#define CS_LEFT   1
#define CS_RIGHT  2
#define CS_BOTTOM 4
#define CS_TOP    8

static int cs_code(double x, double y, double xmin, double ymin,
                   double xmax, double ymax)
{
    int c = CS_INSIDE;
    if (x < xmin)      c |= CS_LEFT;
    else if (x > xmax) c |= CS_RIGHT;
    if (y < ymin)      c |= CS_TOP;
    else if (y > ymax) c |= CS_BOTTOM;
    return c;
}

static int clip_line(double *x0, double *y0, double *x1, double *y1,
                     double xmin, double ymin, double xmax, double ymax)
{
    int c0 = cs_code(*x0, *y0, xmin, ymin, xmax, ymax);
    int c1 = cs_code(*x1, *y1, xmin, ymin, xmax, ymax);
    for (int iter = 0; iter < 8; iter++) {
        if (!(c0 | c1)) return 1;       /* both inside */
        if (c0 & c1)    return 0;       /* both outside, same side */
        int co = c0 ? c0 : c1;
        double x = 0, y = 0;
        double dx = *x1 - *x0, dy = *y1 - *y0;
        if      (co & CS_TOP)    { x = *x0 + dx * (ymin - *y0) / dy; y = ymin; }
        else if (co & CS_BOTTOM) { x = *x0 + dx * (ymax - *y0) / dy; y = ymax; }
        else if (co & CS_RIGHT)  { y = *y0 + dy * (xmax - *x0) / dx; x = xmax; }
        else if (co & CS_LEFT)   { y = *y0 + dy * (xmin - *x0) / dx; x = xmin; }
        if (co == c0) { *x0 = x; *y0 = y; c0 = cs_code(x, y, xmin, ymin, xmax, ymax); }
        else          { *x1 = x; *y1 = y; c1 = cs_code(x, y, xmin, ymin, xmax, ymax); }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Stroke rendering is now routed through the braille canvas at subpixel
 * (2x4 per cell) resolution. Line primitives take *cell-space* float coords
 * (as produced by the projector), scale up to pixel space, clip, and call
 * bc_line. The old character-mode draw_line / slope-aware glyph picking is
 * gone — braille dots look dramatically better at every zoom level.
 * -------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Polygon scanline fill. Collects edges from all rings of the feature,
 * then for each scan row computes intersections and fills between pairs
 * (even-odd rule — naturally handles holes). Edges are in cell-space,
 * already projected.
 * -------------------------------------------------------------------------*/

typedef struct {
    float x0, y0, x1, y1; /* y0 <= y1 by construction */
} Edge;

static int edge_cmp_x(const void *a, const void *b)
{
    float fa = *(const float*)a, fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
}

static void fill_feature(WINDOW *win, MapView *mv, const Projector *p,
                         const GeoFeature *feat, const LayerStyle *style)
{
    int total_pts = 0;
    for (int r = 0; r < feat->nrings; r++) total_pts += feat->rings[r].npoints;
    if (total_pts < 3) return;

    /* Project all vertices once, flattened with edges from each ring. */
    Edge *edges = malloc((size_t)total_pts * sizeof(Edge));
    if (!edges) return;
    int ne = 0;

    float miny = 1e9f, maxy = -1e9f;

    for (int r = 0; r < feat->nrings; r++) {
        const GeoRing *ring = &feat->rings[r];
        if (ring->npoints < 3) continue;
        double px, py, qx, qy;
        project_at(mv, p,
                   ring->coords[1], ring->coords[0], &px, &py);
        float first_x = (float)px, first_y = (float)py;
        float prev_x = first_x, prev_y = first_y;
        for (int i = 1; i < ring->npoints; i++) {
            project_at(mv, p,
                       ring->coords[i * 2 + 1], ring->coords[i * 2],
                       &qx, &qy);
            float cx = (float)qx, cy = (float)qy;
            float y0 = prev_y, y1 = cy;
            float x0 = prev_x, x1 = cx;
            if (y0 != y1) {
                Edge e;
                if (y0 < y1) { e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1; }
                else         { e.x0 = x1; e.y0 = y1; e.x1 = x0; e.y1 = y0; }
                edges[ne++] = e;
                if (e.y0 < miny) miny = e.y0;
                if (e.y1 > maxy) maxy = e.y1;
            }
            prev_x = cx; prev_y = cy;
        }
        /* Close ring if not already */
        if (prev_x != first_x || prev_y != first_y) {
            float y0 = prev_y, y1 = first_y;
            float x0 = prev_x, x1 = first_x;
            if (y0 != y1) {
                Edge e;
                if (y0 < y1) { e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1; }
                else         { e.x0 = x1; e.y0 = y1; e.x1 = x0; e.y1 = y0; }
                edges[ne++] = e;
                if (e.y0 < miny) miny = e.y0;
                if (e.y1 > maxy) maxy = e.y1;
            }
        }
    }

    if (ne == 0) { free(edges); return; }

    int y_lo = (int)floorf(miny);
    int y_hi = (int)ceilf(maxy);
    if (y_lo < 0) y_lo = 0;
    int mx, my;
    getmaxyx(win, my, mx);
    if (y_hi > my - 1) y_hi = my - 1;

    float *xs = malloc((size_t)ne * sizeof(float));
    if (!xs) { free(edges); return; }

    for (int y = y_lo; y <= y_hi; y++) {
        float yf = (float)y + 0.5f;
        int nx = 0;
        for (int i = 0; i < ne; i++) {
            const Edge *e = &edges[i];
            /* Half-open at top: y0 <= yf < y1 */
            if (e->y0 <= yf && yf < e->y1) {
                float t = (yf - e->y0) / (e->y1 - e->y0);
                xs[nx++] = e->x0 + t * (e->x1 - e->x0);
            }
        }
        if (nx < 2) continue;
        qsort(xs, (size_t)nx, sizeof(float), edge_cmp_x);
        for (int i = 0; i + 1 < nx; i += 2) {
            int xa = (int)ceilf(xs[i]);
            int xb = (int)floorf(xs[i + 1]);
            if (xa < 0) xa = 0;
            if (xb > mx - 1) xb = mx - 1;
            for (int x = xa; x <= xb; x++) {
                put_cell(win, x, y, style->fill_ch, style->cp, style->attr);
            }
        }
    }

    free(xs);
    free(edges);
}

/* Draw a clipped segment. ax/ay/bx/by are float *cell* coords from the
 * projector. We scale to pixel coords (×2, ×4), clip against the braille
 * canvas pixel bounds, round, and call bc_line. */
static void draw_segment_clipped(WINDOW *win, double ax, double ay,
                                 double bx, double by,
                                 const LayerStyle *style)
{
    (void)win;
    if (!g_bc.dots) return;
    double x0 = ax * 2.0, y0 = ay * 4.0;
    double x1 = bx * 2.0, y1 = by * 4.0;
    if (!clip_line(&x0, &y0, &x1, &y1,
                   0.0, 0.0,
                   (double)(g_bc.pw - 1), (double)(g_bc.ph - 1)))
        return;
    int ix0 = (int)lround(x0), iy0 = (int)lround(y0);
    int ix1 = (int)lround(x1), iy1 = (int)lround(y1);
    bc_line(&g_bc, ix0, iy0, ix1, iy1, style->cp, style->attr);
}

static void stroke_feature(WINDOW *win, MapView *mv, const Projector *p,
                           const GeoFeature *feat, const LayerStyle *style)
{
    for (int r = 0; r < feat->nrings; r++) {
        const GeoRing *ring = &feat->rings[r];
        if (ring->npoints < 2) continue;
        double px, py, qx, qy;
        project_at(mv, p, ring->coords[1], ring->coords[0], &px, &py);
        double prev_x = px, prev_y = py;
        /* Decimate in pixel space: skip segments that start and end in the
         * same braille dot. At high zoom nothing gets skipped; at low zoom
         * this throws away a ton of sub-dot noise from oversampled data. */
        int prev_px = (int)lround(px * 2.0), prev_py = (int)lround(py * 4.0);
        for (int i = 1; i < ring->npoints; i++) {
            project_at(mv, p,
                       ring->coords[i * 2 + 1], ring->coords[i * 2],
                       &qx, &qy);
            int cpx = (int)lround(qx * 2.0), cpy = (int)lround(qy * 4.0);
            if (cpx != prev_px || cpy != prev_py) {
                draw_segment_clipped(win, prev_x, prev_y, qx, qy, style);
                prev_x = qx; prev_y = qy;
                prev_px = cpx; prev_py = cpy;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public entry points.
 * -------------------------------------------------------------------------*/

void render_clear_water(WINDOW *win)
{
    int mx, my;
    getmaxyx(win, my, mx);
    for (int r = 0; r < my; r++) {
        for (int c = 0; c < mx; c++) {
            /* Space with water color pair — avoids bottom-right cursor issue. */
            if (c == mx - 1 && r == my - 1) continue;
            mvwaddch(win, r, c, ' ' | COLOR_PAIR(CP_WATER));
        }
    }
}

void render_graticule(WINDOW *win, MapView *mv)
{
    /* Only bother at low zooms where the world is sparse. At higher zooms
     * the graticule is visual noise. */
    if (mv->zoom > 5) return;

    int mx, my;
    getmaxyx(win, my, mx);
    double center_px, center_py;
    geo_latlon_to_pixel(mv->center_lat, mv->center_lon, mv->zoom,
                        &center_px, &center_py);
    double char_w = CELL_SUBPX_W, char_h = CELL_SUBPX_H;
    int n = 1 << mv->zoom;

    /* Grid step grows with zoom-out to keep lines visible. */
    double step = (mv->zoom <= 3) ? 30.0 : 10.0;

    for (int r = 0; r < my; r++) {
        for (int c = 0; c < mx; c++) {
            if (c == mx - 1 && r == my - 1) continue;
            double px = center_px + (c - mx / 2.0) * char_w;
            double py = center_py + (r - my / 2.0) * char_h;
            double lon = px / (n * 256.0) * 360.0 - 180.0;
            double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * py / (n * 256.0))));
            double lat = lat_rad * 180.0 / M_PI;
            double glat = fmod(fabs(lat) + step * 0.5, step);
            double glon = fmod(fabs(lon) + step * 0.5, step);
            if (glat < 0.3 || glon < 0.3) {
                put_cell(win, c, r, '.', CP_GRID, A_DIM);
            }
        }
    }
}

/* Run a layer through one projector pass (handles one lon_offset). */
static void render_polygon_pass(WINDOW *win, MapView *mv, Projector *p,
                                const GeoLayer *layer, const LayerStyle *style,
                                const GeoBBox *vb)
{
    for (int i = 0; i < layer->nfeatures; i++) {
        const GeoFeature *f = &layer->features[i];
        if (f->type != GEOM_POLYGON) continue;
        if (!bbox_intersects(&f->bbox, vb)) continue;
        fill_feature(win, mv, p, f, style);
        /* Outline the polygon too for a crisp edge if a stroke char is set. */
        if (style->stroke_ch) stroke_feature(win, mv, p, f, style);
    }
}

static void render_line_pass(WINDOW *win, MapView *mv, Projector *p,
                             const GeoLayer *layer, const LayerStyle *style,
                             const GeoBBox *vb)
{
    for (int i = 0; i < layer->nfeatures; i++) {
        const GeoFeature *f = &layer->features[i];
        if (f->type != GEOM_LINESTRING && f->type != GEOM_POLYGON) continue;
        if (!bbox_intersects(&f->bbox, vb)) continue;
        stroke_feature(win, mv, p, f, style);
    }
}

/* How many lon-offset passes does this viewport need?
 * Returns bitmask over {pass 0: base, pass 1: -360, pass 2: +360}.
 * Only worth doing multi-pass near the antimeridian. */
static int needed_passes(MapView *mv)
{
    GeoBBox v0; viewport_bbox(mv, 0.0, &v0);
    int mask = 1; /* base pass always */
    /* Viewport extends west of -180 → need the +360 pass so we pull data
     * from east lons into view. */
    if (v0.min_lon < -180.0f) mask |= 4;
    /* Viewport extends east of +180 → need the -360 pass. */
    if (v0.max_lon >  180.0f) mask |= 2;
    return mask;
}

void render_polygon_layer(WINDOW *win, MapView *mv, const GeoLayer *layer,
                          const LayerStyle *style)
{
    if (!layer || layer->nfeatures == 0) return;
    int mask = needed_passes(mv);
    double offs[3] = {0.0, -360.0, 360.0};
    for (int k = 0; k < 3; k++) {
        if (!(mask & (1 << k))) continue;
        Projector p; projector_init(&p, mv, offs[k]);
        GeoBBox vb; viewport_bbox(mv, offs[k], &vb);
        render_polygon_pass(win, mv, &p, layer, style, &vb);
    }
}

void render_line_layer(WINDOW *win, MapView *mv, const GeoLayer *layer,
                       const LayerStyle *style)
{
    if (!layer || layer->nfeatures == 0) return;
    int mask = needed_passes(mv);
    double offs[3] = {0.0, -360.0, 360.0};
    for (int k = 0; k < 3; k++) {
        if (!(mask & (1 << k))) continue;
        Projector p; projector_init(&p, mv, offs[k]);
        GeoBBox vb; viewport_bbox(mv, offs[k], &vb);
        render_line_pass(win, mv, &p, layer, style, &vb);
    }
}

/* Sort features by rank ascending so more prominent cities are drawn first
 * and win the label-collision arbitration. */
static int place_cmp(const void *a, const void *b)
{
    const GeoFeature *fa = *(const GeoFeature * const *)a;
    const GeoFeature *fb = *(const GeoFeature * const *)b;
    return fa->rank - fb->rank;
}

void render_places_layer(WINDOW *win, MapView *mv, const GeoLayer *layer)
{
    if (!layer || layer->nfeatures == 0) return;
    Projector p; projector_init(&p, mv, 0.0);
    GeoBBox vb; viewport_bbox(mv, 0.0, &vb);

    /* Rank cutoff: at low zoom, only show prominent cities. */
    int rank_cutoff;
    if      (mv->zoom <= 3)  rank_cutoff = 2;
    else if (mv->zoom <= 5)  rank_cutoff = 4;
    else if (mv->zoom <= 7)  rank_cutoff = 6;
    else if (mv->zoom <= 10) rank_cutoff = 8;
    else                     rank_cutoff = 20;

    /* Gather visible candidates into a pointer array, then sort by rank so
     * big cities claim their labels first. */
    int cap = layer->nfeatures;
    const GeoFeature **cand = malloc((size_t)cap * sizeof(GeoFeature*));
    if (!cand) return;
    int nc = 0;
    for (int i = 0; i < layer->nfeatures; i++) {
        const GeoFeature *f = &layer->features[i];
        if (f->type != GEOM_POINT) continue;
        if (f->rank > rank_cutoff) continue;
        if (!bbox_intersects(&f->bbox, &vb)) continue;
        cand[nc++] = f;
    }
    qsort(cand, (size_t)nc, sizeof(GeoFeature*), place_cmp);

    /* Label occupancy bitmap — one bit per cell, set as labels are placed.
     * Lets us drop a label whose cells overlap an existing one. */
    int mx, my;
    getmaxyx(win, my, mx);
    int occ_sz = mx * my;
    unsigned char *occ = calloc((size_t)occ_sz, 1);
    if (!occ) { free(cand); return; }

    for (int k = 0; k < nc; k++) {
        const GeoFeature *f = cand[k];
        double cx, cy;
        project_at(mv, &p, f->rings[0].coords[1], f->rings[0].coords[0],
                   &cx, &cy);
        int x = (int)lround(cx), y = (int)lround(cy);
        if (x < 0 || y < 0 || x >= mx || y >= my) continue;

        /* Marker — scale with rank so NYC > Phoenix > Akron visually. */
        int ch;
        int attr = A_BOLD;
        if      (f->rank <= 1) { ch = '@'; }
        else if (f->rank <= 3) { ch = 'O'; }
        else if (f->rank <= 6) { ch = 'o'; }
        else                   { ch = '.'; attr = 0; }
        put_cell(win, x, y, ch, CP_MARKER, attr);
        occ[y * mx + x] = 1;

        /* Label. Only attempt at mid+ zoom. */
        if (mv->zoom < 5 || !f->name[0]) continue;
        int name_len = (int)strlen(f->name);
        /* Try placing label to the right first, then left, then below. */
        int tries[3][2] = { {x + 2, y}, {x - name_len - 1, y}, {x + 2, y + 1} };
        for (int t = 0; t < 3; t++) {
            int lx = tries[t][0], ly = tries[t][1];
            if (ly < 0 || ly >= my) continue;
            if (lx < 0 || lx + name_len >= mx - 1) continue;
            int clash = 0;
            for (int q = 0; q < name_len; q++) {
                if (occ[ly * mx + (lx + q)]) { clash = 1; break; }
            }
            if (clash) continue;
            /* Place it. */
            for (int q = 0; q < name_len; q++) {
                int cx2 = lx + q;
                if (cx2 == mx - 1 && ly == my - 1) break;
                mvwaddch(win, ly, cx2,
                         (chtype)(unsigned char)f->name[q] |
                         COLOR_PAIR(CP_MARKER));
                occ[ly * mx + cx2] = 1;
            }
            break;
        }
    }
    free(occ);
    free(cand);
}

void render_polyline_points(WINDOW *win, MapView *mv,
                            const MapPoint *pts, int npts,
                            int ch, int cp, int attr)
{
    if (!pts || npts < 2) return;
    Projector p; projector_init(&p, mv, 0.0);
    LayerStyle style = {0};
    style.cp = cp;
    style.attr = attr;
    style.stroke_ch = ch;
    style.use_slope = 0;

    double px, py, qx, qy;
    project_at(mv, &p, pts[0].lat, pts[0].lon, &px, &py);
    for (int i = 1; i < npts; i++) {
        project_at(mv, &p, pts[i].lat, pts[i].lon, &qx, &qy);
        draw_segment_clipped(win, px, py, qx, qy, &style);
        px = qx; py = qy;
    }
}
