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

/* Download binary data (e.g., PNG tiles). Returns 0 on success. */
int http_get_binary(const char *url, HttpBuffer *buf);

/* Free an HttpBuffer's contents */
void http_buffer_free(HttpBuffer *buf);

#endif /* HTTP_H */