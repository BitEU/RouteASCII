#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* Dynamic buffer for HTTP responses */
typedef struct {
    char  *data;
    size_t size;
    size_t capacity;
} HttpBuffer;

/* Initialize the HTTP subsystem (call once at startup) */
void http_init(void);

/* Cleanup the HTTP subsystem (call once at shutdown) */
void http_cleanup(void);

/* Perform a GET request. Returns 0 on success, -1 on failure.
   Caller must call http_buffer_free() on buf after use. */
int http_get(const char *url, HttpBuffer *buf);

/* Perform a GET request with explicit timeout settings (seconds).
    timeout_s <= 0 means use library defaults. connect_timeout_s <= 0 means use defaults. */
int http_get_timeout(const char *url, HttpBuffer *buf,
                            long timeout_s, long connect_timeout_s);

/* Download binary data (e.g., PNG tiles). Returns 0 on success. */
int http_get_binary(const char *url, HttpBuffer *buf);

/* Free an HttpBuffer's contents */
void http_buffer_free(HttpBuffer *buf);

/* Human-readable description of the last HTTP error, or empty string. */
const char *http_last_error(void);

#endif /* HTTP_H */