#include "mvt.h"
#include "geodata.h"

#include <zlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * Minimal protobuf wire-format reader.
 *
 * Protobuf encodes each field as a tag (field_number << 3 | wire_type)
 * followed by the value. We only need three wire types:
 *   0 = varint        (int32/int64/uint32/uint64/bool/enum)
 *   2 = length-delim  (string/bytes/embedded messages/packed repeated)
 *   5 = fixed32       (fixed32, float)
 *
 * This isn't a generic protobuf library — it's a hand-coded decoder for
 * the specific shape of vector_tile.proto, which is tiny (Tile, Layer,
 * Feature, Value — four messages total).
 *
 * For reference, the MVT schema:
 *
 *   message Tile {
 *     repeated Layer layers = 3;
 *   }
 *   message Layer {
 *     required uint32 version = 15;
 *     required string name = 1;
 *     repeated Feature features = 2;
 *     repeated string keys = 3;
 *     repeated Value values = 4;
 *     optional uint32 extent = 5;           // default 4096
 *   }
 *   message Feature {
 *     optional uint64 id = 1;
 *     repeated uint32 tags = 2 [packed=true];
 *     optional GeomType type = 3;           // 1=POINT 2=LINESTRING 3=POLYGON
 *     repeated uint32 geometry = 4 [packed=true];
 *   }
 *   message Value {
 *     optional string string_value = 1;
 *     optional float  float_value  = 2;
 *     optional double double_value = 3;
 *     optional int64  int_value    = 4;
 *     optional uint64 uint_value   = 5;
 *     optional sint64 sint_value   = 6;
 *     optional bool   bool_value   = 7;
 *   }
 * ---------------------------------------------------------------------*/

typedef struct {
    const unsigned char *p;
    const unsigned char *end;
} PbReader;

/* Read a protobuf varint (up to 10 bytes). Returns 0 on success, -1 on
 * truncation. */
static int pb_varint(PbReader *r, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (r->p < r->end) {
        unsigned char b = *r->p++;
        v |= ((uint64_t)(b & 0x7f)) << shift;
        if ((b & 0x80) == 0) { *out = v; return 0; }
        shift += 7;
        if (shift >= 64) return -1;
    }
    return -1;
}

/* Skip a single field given its wire type — used to ignore fields we
 * don't care about. */
static int pb_skip(PbReader *r, int wire_type)
{
    uint64_t v;
    switch (wire_type) {
    case 0: /* varint */
        return pb_varint(r, &v);
    case 1: /* fixed64 */
        if (r->end - r->p < 8) return -1;
        r->p += 8;
        return 0;
    case 2: /* length-delimited */
        if (pb_varint(r, &v) < 0) return -1;
        if ((uint64_t)(r->end - r->p) < v) return -1;
        r->p += v;
        return 0;
    case 5: /* fixed32 */
        if (r->end - r->p < 4) return -1;
        r->p += 4;
        return 0;
    default:
        return -1;
    }
}

/* Zigzag decoding for sint32/sint64 and MVT command parameters. */
static int32_t zigzag32(uint32_t n) { return (int32_t)((n >> 1) ^ (~(n & 1) + 1)); }

/* ---------------------------------------------------------------------------
 * Gzip decompression via zlib. MBTiles blobs are usually gzip-compressed
 * MVT; sometimes raw MVT for uncompressed tilesets. Handle both.
 * ---------------------------------------------------------------------*/
static int maybe_gunzip(const unsigned char *data, size_t size,
                        unsigned char **out, size_t *out_size)
{
    /* Gzip magic: 1f 8b. If missing, pass through. */
    if (size < 2 || data[0] != 0x1f || data[1] != 0x8b) {
        *out = malloc(size);
        if (!*out) return -1;
        memcpy(*out, data, size);
        *out_size = size;
        return 0;
    }

    z_stream zs = {0};
    zs.next_in = (Bytef *)data;
    zs.avail_in = (uInt)size;
    /* 16 + MAX_WBITS tells inflate to decode gzip framing. */
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return -1;

    /* Heuristic: assume ~8x expansion, grow as needed. */
    size_t cap = size * 8;
    if (cap < 65536) cap = 65536;
    unsigned char *buf = malloc(cap);
    if (!buf) { inflateEnd(&zs); return -1; }
    size_t used = 0;

    for (;;) {
        zs.next_out = buf + used;
        zs.avail_out = (uInt)(cap - used);
        int rc = inflate(&zs, Z_NO_FLUSH);
        used = cap - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { inflateEnd(&zs); free(buf); return -1; }
        if (zs.avail_out == 0) {
            size_t nc = cap * 2;
            unsigned char *nb = realloc(buf, nc);
            if (!nb) { inflateEnd(&zs); free(buf); return -1; }
            buf = nb;
            cap = nc;
        }
    }
    inflateEnd(&zs);
    *out = buf;
    *out_size = used;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Tile coordinate -> WGS84 lon/lat.
 *
 * For a tile at (z, tx, ty) the tile-local geometry coordinate (lx, ly)
 * where 0 <= lx, ly < extent maps to world slippy pixels:
 *   wx = (tx + lx/extent) / (1 << z)
 *   wy = (ty + ly/extent) / (1 << z)
 * and then to lon/lat via the standard inverse-mercator.
 * ---------------------------------------------------------------------*/
typedef struct {
    int z, tx, ty;
    double inv_extent;
    double tile_n;  /* 1 << z */
} TileXform;

static void tile_xform_init(TileXform *xf, int z, int tx, int ty, int extent)
{
    xf->z = z;
    xf->tx = tx;
    xf->ty = ty;
    xf->inv_extent = 1.0 / (double)extent;
    xf->tile_n = (double)(1 << z);
}

static inline void tile_local_to_lonlat(const TileXform *xf,
                                        int lx, int ly,
                                        float *lon, float *lat)
{
    double wx = ((double)xf->tx + (double)lx * xf->inv_extent) / xf->tile_n;
    double wy = ((double)xf->ty + (double)ly * xf->inv_extent) / xf->tile_n;
    double lon_d = wx * 360.0 - 180.0;
    double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * wy)));
    *lon = (float)lon_d;
    *lat = (float)(lat_rad * 180.0 / M_PI);
}

/* ---------------------------------------------------------------------------
 * Per-layer state during decode.
 *
 * Keys and string values are small arrays of malloc'd strings; we keep
 * them as zero-copy pointers into the gunzipped buffer plus length.
 * ---------------------------------------------------------------------*/

typedef struct {
    const char *ptr;
    size_t      len;
} StrView;

typedef struct {
    /* Polymorphic OMT value — we only care about strings and ints in
     * practice (the `class`, `subclass`, `rank` fields). */
    int         kind;   /* 0 none, 1 string, 2 int, 3 double */
    StrView     s;
    int64_t     i;
    double      d;
} MvtValue;

#define MAX_KEYS   256
#define MAX_VALUES 4096

typedef struct {
    StrView   name;
    int       version;
    int       extent;
    StrView   keys[MAX_KEYS];
    int       nkeys;
    MvtValue  values[MAX_VALUES];
    int       nvalues;
} LayerHdr;

static int strview_eq(const StrView *s, const char *lit)
{
    size_t n = strlen(lit);
    return s->len == n && memcmp(s->ptr, lit, n) == 0;
}

/* ---------------------------------------------------------------------------
 * Look up "key: value" for a feature, returning the value pointer or NULL.
 * Feature tags are an even-length packed varint array [k0, v0, k1, v1, ...]
 * indexing into the layer's keys[] and values[].
 * ---------------------------------------------------------------------*/
static const MvtValue *feat_tag(const LayerHdr *lh,
                                const uint32_t *tags, int ntags,
                                const char *key)
{
    for (int i = 0; i + 1 < ntags; i += 2) {
        uint32_t ki = tags[i], vi = tags[i + 1];
        if (ki >= (uint32_t)lh->nkeys || vi >= (uint32_t)lh->nvalues) continue;
        if (strview_eq(&lh->keys[ki], key)) return &lh->values[vi];
    }
    return NULL;
}

/* Classify a transportation feature's `class` field into our OsmLayerId. */
static int classify_transport(const StrView *s)
{
    if (!s) return -1;
    if (strview_eq(s, "motorway"))  return OSM_LAYER_ROAD_MOTORWAY;
    if (strview_eq(s, "trunk"))     return OSM_LAYER_ROAD_TRUNK;
    if (strview_eq(s, "primary"))   return OSM_LAYER_ROAD_PRIMARY;
    if (strview_eq(s, "secondary")) return OSM_LAYER_ROAD_SECONDARY;
    if (strview_eq(s, "tertiary"))  return OSM_LAYER_ROAD_TERTIARY;
    if (strview_eq(s, "minor"))     return OSM_LAYER_ROAD_MINOR;
    if (strview_eq(s, "service"))   return OSM_LAYER_ROAD_SERVICE;
    if (strview_eq(s, "track"))     return OSM_LAYER_ROAD_SERVICE;
    if (strview_eq(s, "path"))      return OSM_LAYER_ROAD_PATH;
    /* OMT also emits "raceway", "busway", "rail", "aerialway" etc. —
     * we drop these. */
    return -1;
}

/* ---------------------------------------------------------------------------
 * Feature geometry decoder.
 *
 * MVT geometries are a packed uint32 array of "command integers". Each
 * command integer is (cmd << 3) | (count << 0). Commands:
 *   1 MoveTo   — count pairs of (dx, dy) zigzag deltas
 *   2 LineTo   — count pairs of (dx, dy)
 *   7 ClosePath — count=1, no parameters
 *
 * For POLYGON geometries, rings come one after another — a MoveTo starts
 * a new ring. We collect them into a single GeoFeature with multiple
 * GeoRings. For LINESTRING, each MoveTo is a new sub-line — we emit one
 * GeoFeature per sub-line so bbox culling stays tight.
 *
 * For POINT, a MoveTo with count N produces N independent points —
 * usually just one. We emit a GEOM_POINT feature.
 * ---------------------------------------------------------------------*/

/* Append a completed linestring ring as a GeoFeature of type LINESTRING. */
static void push_linestring(GeoLayer *out, const TileXform *xf,
                            const int *lcx, const int *lcy, int n,
                            const char *name)
{
    if (n < 2) return;
    GeoFeature f = {0};
    f.type = GEOM_LINESTRING;
    f.bbox.min_lat =  90.0f; f.bbox.max_lat = -90.0f;
    f.bbox.min_lon = 180.0f; f.bbox.max_lon = -180.0f;
    f.rings = calloc(1, sizeof(GeoRing));
    if (!f.rings) return;
    f.rings[0].coords = malloc((size_t)n * 2 * sizeof(float));
    if (!f.rings[0].coords) { free(f.rings); return; }
    for (int i = 0; i < n; i++) {
        float lon, lat;
        tile_local_to_lonlat(xf, lcx[i], lcy[i], &lon, &lat);
        f.rings[0].coords[i * 2 + 0] = lon;
        f.rings[0].coords[i * 2 + 1] = lat;
        if (lat < f.bbox.min_lat) f.bbox.min_lat = lat;
        if (lat > f.bbox.max_lat) f.bbox.max_lat = lat;
        if (lon < f.bbox.min_lon) f.bbox.min_lon = lon;
        if (lon > f.bbox.max_lon) f.bbox.max_lon = lon;
    }
    f.rings[0].npoints = n;
    f.nrings = 1;
    if (name) { strncpy(f.name, name, sizeof(f.name) - 1); }
    geolayer_add(out, f);
}

/* Append a point feature with optional name. */
static void push_point(GeoLayer *out, const TileXform *xf,
                       int lx, int ly, const char *name, int rank)
{
    GeoFeature f = {0};
    f.type = GEOM_POINT;
    f.rings = calloc(1, sizeof(GeoRing));
    if (!f.rings) return;
    f.rings[0].coords = malloc(2 * sizeof(float));
    if (!f.rings[0].coords) { free(f.rings); return; }
    float lon, lat;
    tile_local_to_lonlat(xf, lx, ly, &lon, &lat);
    f.rings[0].coords[0] = lon;
    f.rings[0].coords[1] = lat;
    f.rings[0].npoints = 1;
    f.nrings = 1;
    f.bbox.min_lat = f.bbox.max_lat = lat;
    f.bbox.min_lon = f.bbox.max_lon = lon;
    f.rank = rank;
    if (name) { strncpy(f.name, name, sizeof(f.name) - 1); }
    geolayer_add(out, f);
}

/* Append a polygon (possibly multi-ring) to a layer. Rings are stored
 * in the order they arrive — the caller (the polygon fill code in
 * render.c) already handles even-odd fill with holes, so we don't need
 * to care about winding direction here. */
typedef struct {
    int *cx, *cy;
    int  n, cap;
} RingBuf;

static void ringbuf_push(RingBuf *rb, int x, int y)
{
    if (rb->n >= rb->cap) {
        int nc = rb->cap ? rb->cap * 2 : 32;
        int *nx = realloc(rb->cx, (size_t)nc * sizeof(int));
        int *ny = realloc(rb->cy, (size_t)nc * sizeof(int));
        if (!nx || !ny) { free(nx); free(ny); return; }
        rb->cx = nx; rb->cy = ny; rb->cap = nc;
    }
    rb->cx[rb->n] = x;
    rb->cy[rb->n] = y;
    rb->n++;
}

/* Decode a feature's geometry command stream into the layer. */
static void decode_geometry(GeoLayer *out, const TileXform *xf,
                            int geom_type,
                            const uint32_t *cmds, int ncmds,
                            const char *name, int rank)
{
    int cx = 0, cy = 0;
    int i = 0;

    if (geom_type == 1) {
        /* POINT — MoveTo with count>=1 parameters. Each produces a point. */
        while (i < ncmds) {
            uint32_t cmd = cmds[i++];
            int op = cmd & 0x7;
            int cnt = cmd >> 3;
            if (op != 1) break;
            for (int k = 0; k < cnt && i + 1 < ncmds; k++) {
                cx += zigzag32(cmds[i++]);
                cy += zigzag32(cmds[i++]);
                push_point(out, xf, cx, cy, name, rank);
            }
        }
        return;
    }

    if (geom_type == 2) {
        /* LINESTRING — one or more MoveTo + LineTo pairs. Emit each
         * sub-line as its own GeoFeature. */
        int *lcx = NULL, *lcy = NULL;
        int ln = 0, lcap = 0;
        while (i < ncmds) {
            uint32_t cmd = cmds[i++];
            int op = cmd & 0x7;
            int cnt = cmd >> 3;
            if (op == 1) {
                /* Flush current sub-line, start new. */
                if (ln >= 2) push_linestring(out, xf, lcx, lcy, ln, name);
                ln = 0;
                if (cnt >= 1 && i + 1 < ncmds) {
                    cx += zigzag32(cmds[i++]);
                    cy += zigzag32(cmds[i++]);
                    if (lcap == 0) {
                        lcap = 32;
                        lcx = malloc((size_t)lcap * sizeof(int));
                        lcy = malloc((size_t)lcap * sizeof(int));
                        if (!lcx || !lcy) { free(lcx); free(lcy); return; }
                    }
                    lcx[ln] = cx; lcy[ln] = cy; ln++;
                }
            } else if (op == 2) {
                for (int k = 0; k < cnt && i + 1 < ncmds; k++) {
                    cx += zigzag32(cmds[i++]);
                    cy += zigzag32(cmds[i++]);
                    if (ln >= lcap) {
                        int nc = lcap ? lcap * 2 : 32;
                        int *nx = realloc(lcx, (size_t)nc * sizeof(int));
                        int *ny = realloc(lcy, (size_t)nc * sizeof(int));
                        if (!nx || !ny) { free(nx); free(ny); return; }
                        lcx = nx; lcy = ny; lcap = nc;
                    }
                    lcx[ln] = cx; lcy[ln] = cy; ln++;
                }
            } else {
                break;
            }
        }
        if (ln >= 2) push_linestring(out, xf, lcx, lcy, ln, name);
        free(lcx); free(lcy);
        return;
    }

    if (geom_type == 3) {
        /* POLYGON — MoveTo (1 pair), LineTo (n pairs), ClosePath. Repeats
         * for each ring. Build rings into a temp list and emit as one
         * GeoFeature with multi-ring support. */
        GeoFeature f = {0};
        f.type = GEOM_POLYGON;
        f.bbox.min_lat =  90.0f; f.bbox.max_lat = -90.0f;
        f.bbox.min_lon = 180.0f; f.bbox.max_lon = -180.0f;
        int ring_cap = 0;

        RingBuf cur = {0};
        while (i < ncmds) {
            uint32_t cmd = cmds[i++];
            int op = cmd & 0x7;
            int cnt = cmd >> 3;
            if (op == 1) {
                /* MoveTo starts a new ring. Flush any prior pending points
                 * (shouldn't happen legally, but be defensive). */
                cur.n = 0;
                if (cnt >= 1 && i + 1 < ncmds) {
                    cx += zigzag32(cmds[i++]);
                    cy += zigzag32(cmds[i++]);
                    ringbuf_push(&cur, cx, cy);
                }
            } else if (op == 2) {
                for (int k = 0; k < cnt && i + 1 < ncmds; k++) {
                    cx += zigzag32(cmds[i++]);
                    cy += zigzag32(cmds[i++]);
                    ringbuf_push(&cur, cx, cy);
                }
            } else if (op == 7) {
                /* ClosePath — close and emit the ring. */
                if (cur.n >= 3) {
                    /* Repeat first point at the end to close. */
                    ringbuf_push(&cur, cur.cx[0], cur.cy[0]);
                    if (f.nrings >= ring_cap) {
                        ring_cap = ring_cap ? ring_cap * 2 : 4;
                        GeoRing *nr = realloc(f.rings,
                                              (size_t)ring_cap * sizeof(GeoRing));
                        if (!nr) { free(cur.cx); free(cur.cy); return; }
                        f.rings = nr;
                    }
                    GeoRing *gr = &f.rings[f.nrings];
                    gr->coords = malloc((size_t)cur.n * 2 * sizeof(float));
                    if (!gr->coords) { free(cur.cx); free(cur.cy); return; }
                    gr->npoints = cur.n;
                    for (int k = 0; k < cur.n; k++) {
                        float lon, lat;
                        tile_local_to_lonlat(xf, cur.cx[k], cur.cy[k], &lon, &lat);
                        gr->coords[k * 2 + 0] = lon;
                        gr->coords[k * 2 + 1] = lat;
                        if (lat < f.bbox.min_lat) f.bbox.min_lat = lat;
                        if (lat > f.bbox.max_lat) f.bbox.max_lat = lat;
                        if (lon < f.bbox.min_lon) f.bbox.min_lon = lon;
                        if (lon > f.bbox.max_lon) f.bbox.max_lon = lon;
                    }
                    f.nrings++;
                    cur.n = 0;
                }
            }
        }
        free(cur.cx); free(cur.cy);
        if (f.nrings > 0) {
            if (name) strncpy(f.name, name, sizeof(f.name) - 1);
            geolayer_add(out, f);
        } else {
            free(f.rings);
        }
        return;
    }
}

/* ---------------------------------------------------------------------------
 * Decode a packed varint array (bytes in [p, p+len)) into a freshly
 * malloc'd uint32 array. Returns NULL on truncation.
 * ---------------------------------------------------------------------*/
static uint32_t *decode_packed_u32(const unsigned char *p, size_t len, int *out_n)
{
    /* Upper bound — each varint is at least 1 byte. */
    uint32_t *arr = malloc(len * sizeof(uint32_t));
    if (!arr) return NULL;
    int n = 0;
    const unsigned char *end = p + len;
    while (p < end) {
        uint64_t v = 0;
        int shift = 0;
        while (p < end) {
            unsigned char b = *p++;
            v |= ((uint64_t)(b & 0x7f)) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift >= 64) { free(arr); return NULL; }
        }
        arr[n++] = (uint32_t)v;
    }
    *out_n = n;
    return arr;
}

/* ---------------------------------------------------------------------------
 * Decode one Feature message given the layer header.
 * ---------------------------------------------------------------------*/
static void decode_feature(OsmCache *cache, const LayerHdr *lh,
                           const TileXform *xf, int target_layer_hint,
                           const unsigned char *msg, size_t msg_len)
{
    PbReader r = { msg, msg + msg_len };

    uint32_t *tags = NULL;
    int       ntags = 0;
    int       geom_type = 0;
    uint32_t *geom = NULL;
    int       ngeom = 0;

    while (r.p < r.end) {
        uint64_t tag;
        if (pb_varint(&r, &tag) < 0) goto done;
        int field = (int)(tag >> 3);
        int wire  = (int)(tag & 0x7);

        if (field == 1 && wire == 0) {
            /* id — skip */
            uint64_t v; if (pb_varint(&r, &v) < 0) goto done;
        } else if (field == 2 && wire == 2) {
            /* tags — packed uint32 */
            uint64_t len; if (pb_varint(&r, &len) < 0) goto done;
            if ((uint64_t)(r.end - r.p) < len) goto done;
            free(tags);
            tags = decode_packed_u32(r.p, (size_t)len, &ntags);
            r.p += len;
        } else if (field == 3 && wire == 0) {
            uint64_t v; if (pb_varint(&r, &v) < 0) goto done;
            geom_type = (int)v;
        } else if (field == 4 && wire == 2) {
            uint64_t len; if (pb_varint(&r, &len) < 0) goto done;
            if ((uint64_t)(r.end - r.p) < len) goto done;
            free(geom);
            geom = decode_packed_u32(r.p, (size_t)len, &ngeom);
            r.p += len;
        } else {
            if (pb_skip(&r, wire) < 0) goto done;
        }
    }

    if (!geom || ngeom <= 0 || geom_type == 0) goto done;

    /* Route the feature to the correct output layer. */
    int target = target_layer_hint;
    const char *name = NULL;
    char name_buf[64] = {0};
    int rank = 0;

    if (target_layer_hint == OSM_LAYER_ROAD_MOTORWAY) {
        /* Transportation layer — use `class` field to pick the bucket. */
        const MvtValue *cls = feat_tag(lh, tags, ntags, "class");
        if (!cls || cls->kind != 1) goto done;
        target = classify_transport(&cls->s);
        if (target < 0) goto done;
    } else if (target_layer_hint == OSM_LAYER_PLACE) {
        /* place layer — pull the name and rank for labeling. */
        const MvtValue *nm = feat_tag(lh, tags, ntags, "name");
        if (nm && nm->kind == 1) {
            size_t n = nm->s.len < sizeof(name_buf) - 1
                     ? nm->s.len : sizeof(name_buf) - 1;
            memcpy(name_buf, nm->s.ptr, n);
            name_buf[n] = '\0';
            name = name_buf;
        }
        const MvtValue *rk = feat_tag(lh, tags, ntags, "rank");
        if (rk && rk->kind == 2) rank = (int)rk->i;
    }

    if (target < 0 || target >= OSM_LAYER_COUNT) goto done;
    decode_geometry(&cache->layers[target], xf, geom_type,
                    geom, ngeom, name, rank);

done:
    free(tags);
    free(geom);
}

/* ---------------------------------------------------------------------------
 * Decode one Value message into a MvtValue union.
 * ---------------------------------------------------------------------*/
static void decode_value(MvtValue *out,
                         const unsigned char *msg, size_t msg_len)
{
    PbReader r = { msg, msg + msg_len };
    memset(out, 0, sizeof(*out));
    while (r.p < r.end) {
        uint64_t tag;
        if (pb_varint(&r, &tag) < 0) return;
        int field = (int)(tag >> 3);
        int wire  = (int)(tag & 0x7);
        if (field == 1 && wire == 2) {
            /* string_value */
            uint64_t len; if (pb_varint(&r, &len) < 0) return;
            if ((uint64_t)(r.end - r.p) < len) return;
            out->kind = 1;
            out->s.ptr = (const char *)r.p;
            out->s.len = (size_t)len;
            r.p += len;
        } else if (field == 2 && wire == 5) {
            /* float_value */
            if (r.end - r.p < 4) return;
            float f;
            memcpy(&f, r.p, 4);
            r.p += 4;
            out->kind = 3;
            out->d = f;
        } else if (field == 3 && wire == 1) {
            /* double_value */
            if (r.end - r.p < 8) return;
            double d;
            memcpy(&d, r.p, 8);
            r.p += 8;
            out->kind = 3;
            out->d = d;
        } else if ((field == 4 || field == 5) && wire == 0) {
            /* int_value / uint_value */
            uint64_t v; if (pb_varint(&r, &v) < 0) return;
            out->kind = 2;
            out->i = (int64_t)v;
        } else if (field == 6 && wire == 0) {
            /* sint_value */
            uint64_t v; if (pb_varint(&r, &v) < 0) return;
            out->kind = 2;
            out->i = (int64_t)((v >> 1) ^ (~(v & 1) + 1));
        } else if (field == 7 && wire == 0) {
            /* bool_value */
            uint64_t v; if (pb_varint(&r, &v) < 0) return;
            out->kind = 2;
            out->i = (int64_t)v;
        } else {
            if (pb_skip(&r, wire) < 0) return;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Decode one Layer message.
 * ---------------------------------------------------------------------*/
static void decode_layer(OsmCache *cache, int z, int tx, int ty,
                         const unsigned char *msg, size_t msg_len)
{
    LayerHdr lh = {0};
    lh.extent = 4096;

    /* First pass: scan for name/extent/keys/values so the feature pass
     * has everything it needs. We also collect feature message extents
     * in a small list and decode them after the header is built. */
    const unsigned char **feat_ptr = NULL;
    size_t              *feat_len = NULL;
    int                  nfeat = 0, feat_cap = 0;

    PbReader r = { msg, msg + msg_len };
    while (r.p < r.end) {
        uint64_t tag;
        if (pb_varint(&r, &tag) < 0) goto done;
        int field = (int)(tag >> 3);
        int wire  = (int)(tag & 0x7);

        if (field == 1 && wire == 2) {
            /* name */
            uint64_t len; if (pb_varint(&r, &len) < 0) goto done;
            if ((uint64_t)(r.end - r.p) < len) goto done;
            lh.name.ptr = (const char *)r.p;
            lh.name.len = (size_t)len;
            r.p += len;
        } else if (field == 2 && wire == 2) {
            /* Feature — stash the byte range, decode later. */
            uint64_t len; if (pb_varint(&r, &len) < 0) goto done;
            if ((uint64_t)(r.end - r.p) < len) goto done;
            if (nfeat >= feat_cap) {
                int nc = feat_cap ? feat_cap * 2 : 32;
                const unsigned char **np = realloc(feat_ptr, (size_t)nc * sizeof(*np));
                size_t *nl = realloc(feat_len, (size_t)nc * sizeof(size_t));
                if (!np || !nl) { free(np); free(nl); goto done; }
                feat_ptr = np; feat_len = nl; feat_cap = nc;
            }
            feat_ptr[nfeat] = r.p;
            feat_len[nfeat] = (size_t)len;
            nfeat++;
            r.p += len;
        } else if (field == 3 && wire == 2) {
            /* keys — string */
            uint64_t len; if (pb_varint(&r, &len) < 0) goto done;
            if ((uint64_t)(r.end - r.p) < len) goto done;
            if (lh.nkeys < MAX_KEYS) {
                lh.keys[lh.nkeys].ptr = (const char *)r.p;
                lh.keys[lh.nkeys].len = (size_t)len;
                lh.nkeys++;
            }
            r.p += len;
        } else if (field == 4 && wire == 2) {
            /* values — embedded Value message */
            uint64_t len; if (pb_varint(&r, &len) < 0) goto done;
            if ((uint64_t)(r.end - r.p) < len) goto done;
            if (lh.nvalues < MAX_VALUES) {
                decode_value(&lh.values[lh.nvalues], r.p, (size_t)len);
                lh.nvalues++;
            }
            r.p += len;
        } else if (field == 5 && wire == 0) {
            /* extent */
            uint64_t v; if (pb_varint(&r, &v) < 0) goto done;
            lh.extent = (int)v;
        } else if (field == 15 && wire == 0) {
            /* version */
            uint64_t v; if (pb_varint(&r, &v) < 0) goto done;
            lh.version = (int)v;
        } else {
            if (pb_skip(&r, wire) < 0) goto done;
        }
    }

    /* Decide which OsmLayerId this MVT layer maps to (if any). We use a
     * "hint" value that tells the feature decoder how to finalize
     * classification — for transportation we need to look at each
     * feature's `class` tag, but for waterway/water/place/boundary the
     * mapping is fixed by layer name. */
    int hint = -1;
    if (strview_eq(&lh.name, "transportation"))        hint = OSM_LAYER_ROAD_MOTORWAY;
    else if (strview_eq(&lh.name, "waterway"))         hint = OSM_LAYER_WATERWAY;
    else if (strview_eq(&lh.name, "water"))            hint = OSM_LAYER_WATER;
    else if (strview_eq(&lh.name, "place"))            hint = OSM_LAYER_PLACE;
    else if (strview_eq(&lh.name, "boundary"))         hint = OSM_LAYER_BOUNDARY;
    else goto done;

    TileXform xf;
    tile_xform_init(&xf, z, tx, ty, lh.extent);

    for (int i = 0; i < nfeat; i++) {
        decode_feature(cache, &lh, &xf, hint, feat_ptr[i], feat_len[i]);
    }

done:
    free(feat_ptr);
    free(feat_len);
}

/* ---------------------------------------------------------------------------
 * Public entry point — decode a whole Tile message.
 * ---------------------------------------------------------------------*/
int mvt_decode_tile(OsmCache *cache,
                    int z, int x, int y,
                    const unsigned char *data, size_t size)
{
    unsigned char *raw = NULL;
    size_t raw_size = 0;
    if (maybe_gunzip(data, size, &raw, &raw_size) < 0) return -1;

    PbReader r = { raw, raw + raw_size };
    while (r.p < r.end) {
        uint64_t tag;
        if (pb_varint(&r, &tag) < 0) break;
        int field = (int)(tag >> 3);
        int wire  = (int)(tag & 0x7);
        if (field == 3 && wire == 2) {
            /* Layer */
            uint64_t len; if (pb_varint(&r, &len) < 0) break;
            if ((uint64_t)(r.end - r.p) < len) break;
            decode_layer(cache, z, x, y, r.p, (size_t)len);
            r.p += len;
        } else {
            if (pb_skip(&r, wire) < 0) break;
        }
    }

    free(raw);
    return 0;
}
