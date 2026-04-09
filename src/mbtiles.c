#include "mbtiles.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Mbtiles {
    sqlite3      *db;
    sqlite3_stmt *get_stmt;
    int           minzoom;
    int           maxzoom;
};

/* Read a metadata value by name. Returns malloc'd string or NULL. */
static char *read_metadata(sqlite3 *db, const char *key)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT value FROM metadata WHERE name = ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(stmt, 0);
        if (v) {
            size_t n = strlen((const char *)v);
            result = malloc(n + 1);
            if (result) { memcpy(result, v, n + 1); }
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

Mbtiles *mbtiles_open(const char *path)
{
    sqlite3 *db = NULL;
    /* Read-only open with no mutex — we only access from the main thread. */
    if (sqlite3_open_v2(path, &db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                        NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    /* Sanity-check: does the `tiles` table/view exist? MBTiles files
     * sometimes store the table as a view over `map` + `images` for
     * dedup — either way a SELECT against `tiles` works. */
    sqlite3_stmt *check = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM tiles LIMIT 1", -1, &check, NULL)
        != SQLITE_OK) {
        fprintf(stderr, "[mbtiles] %s: no `tiles` table (%s)\n",
                path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_finalize(check);

    /* Pragmas for bulk read performance — this is a read-only DB on
     * local disk, we can be aggressive. */
    sqlite3_exec(db, "PRAGMA cache_size = -65536;",   NULL, NULL, NULL); /* 64MB */
    sqlite3_exec(db, "PRAGMA mmap_size  = 1073741824;", NULL, NULL, NULL); /* 1GB mmap */
    sqlite3_exec(db, "PRAGMA temp_store = MEMORY;",   NULL, NULL, NULL);

    Mbtiles *m = calloc(1, sizeof(*m));
    if (!m) { sqlite3_close(db); return NULL; }
    m->db = db;
    m->minzoom = -1;
    m->maxzoom = -1;

    /* Prepare the hot query once and reuse. */
    const char *sql =
        "SELECT tile_data FROM tiles "
        "WHERE zoom_level = ?1 AND tile_column = ?2 AND tile_row = ?3";
    if (sqlite3_prepare_v2(db, sql, -1, &m->get_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[mbtiles] prepare failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        free(m);
        return NULL;
    }

    /* Pull zoom range from metadata, with a fallback to MAX/MIN query. */
    char *mn = read_metadata(db, "minzoom");
    char *mx = read_metadata(db, "maxzoom");
    if (mn) { m->minzoom = atoi(mn); free(mn); }
    if (mx) { m->maxzoom = atoi(mx); free(mx); }
    if (m->minzoom < 0 || m->maxzoom < 0) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT MIN(zoom_level), MAX(zoom_level) FROM tiles",
                -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                m->minzoom = sqlite3_column_int(s, 0);
                m->maxzoom = sqlite3_column_int(s, 1);
            }
            sqlite3_finalize(s);
        }
    }

    fprintf(stderr, "[mbtiles] opened %s (zoom %d..%d)\n",
            path, m->minzoom, m->maxzoom);
    return m;
}

void mbtiles_close(Mbtiles *m)
{
    if (!m) return;
    if (m->get_stmt) sqlite3_finalize(m->get_stmt);
    if (m->db)       sqlite3_close(m->db);
    free(m);
}

int mbtiles_minzoom(const Mbtiles *m) { return m ? m->minzoom : -1; }
int mbtiles_maxzoom(const Mbtiles *m) { return m ? m->maxzoom : -1; }

int mbtiles_get_tile(Mbtiles *m, int z, int x, int y,
                     unsigned char **out_data, size_t *out_size)
{
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!m || !m->get_stmt) return -1;

    /* XYZ -> TMS Y flip. At zoom z, world height is (1 << z). */
    int tms_y = ((1 << z) - 1) - y;

    sqlite3_reset(m->get_stmt);
    sqlite3_clear_bindings(m->get_stmt);
    sqlite3_bind_int(m->get_stmt, 1, z);
    sqlite3_bind_int(m->get_stmt, 2, x);
    sqlite3_bind_int(m->get_stmt, 3, tms_y);

    int rc = sqlite3_step(m->get_stmt);
    if (rc != SQLITE_ROW) return -1;

    const void *blob = sqlite3_column_blob(m->get_stmt, 0);
    int n = sqlite3_column_bytes(m->get_stmt, 0);
    if (!blob || n <= 0) return -1;

    unsigned char *buf = malloc((size_t)n);
    if (!buf) return -1;
    memcpy(buf, blob, (size_t)n);
    if (out_data) *out_data = buf;
    if (out_size) *out_size = (size_t)n;
    return 0;
}
