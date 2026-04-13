#include "http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define INITIAL_BUF_SIZE 4096
#define USER_AGENT       "RouteASCII/1.0 (TUI Map Viewer)"

static CURL *g_curl = NULL;
static char g_last_error[256] = "";

static void set_last_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    HttpBuffer *buf = (HttpBuffer *)userp;

    if (buf->size + total + 1 > buf->capacity) {
        size_t new_cap = (buf->capacity == 0) ? INITIAL_BUF_SIZE : buf->capacity;
        while (new_cap < buf->size + total + 1)
            new_cap *= 2;
        char *tmp = realloc(buf->data, new_cap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

void http_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
    if (g_curl) {
        curl_easy_setopt(g_curl, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_callback);
        g_last_error[0] = '\0';
        fprintf(stderr, "[HTTP] initialized (timeout=15s connect=10s)\n");
    } else {
        set_last_error("curl_easy_init failed");
        fprintf(stderr, "[HTTP] initialization failed: curl_easy_init returned NULL\n");
    }
}

void http_cleanup(void)
{
    if (g_curl) {
        curl_easy_cleanup(g_curl);
        g_curl = NULL;
        fprintf(stderr, "[HTTP] cleaned up curl handle\n");
    }
    curl_global_cleanup();
}

static int do_get(const char *url, HttpBuffer *buf)
{
    if (!g_curl || !url || !buf) {
        set_last_error("HTTP subsystem not initialized");
        return -1;
    }

    g_last_error[0] = '\0';

    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, buf);

    fprintf(stderr, "[HTTP] GET %s\n", url);

    CURLcode res = curl_easy_perform(g_curl);
    double total_s = 0.0;
    curl_easy_getinfo(g_curl, CURLINFO_TOTAL_TIME, &total_s);
    if (res != CURLE_OK) {
        set_last_error("curl error: %s", curl_easy_strerror(res));
        fprintf(stderr, "[HTTP] Error: %s (url: %s, %.0f ms)\n",
                curl_easy_strerror(res), url, total_s * 1000.0);
        http_buffer_free(buf);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        char preview[96] = "";
        if (buf->data && buf->size > 0) {
            size_t n = buf->size;
            if (n > sizeof(preview) - 1) n = sizeof(preview) - 1;
            memcpy(preview, buf->data, n);
            preview[n] = '\0';
            for (size_t i = 0; preview[i]; i++) {
                if (preview[i] == '\r' || preview[i] == '\n' || preview[i] == '\t') {
                    preview[i] = ' ';
                }
            }
        }
        if (preview[0]) {
            set_last_error("HTTP %ld: %.80s", http_code, preview);
        } else {
            set_last_error("HTTP %ld", http_code);
        }
        fprintf(stderr, "[HTTP] Status %ld for %s (%.0f ms, %zu bytes)\n",
                http_code, url, total_s * 1000.0, buf->size);
        http_buffer_free(buf);
        return -1;
    }

    fprintf(stderr, "[HTTP] OK 200 %s (%.0f ms, %zu bytes)\n",
            url, total_s * 1000.0, buf->size);

    return 0;
}

int http_get(const char *url, HttpBuffer *buf)
{
    return do_get(url, buf);
}

int http_get_binary(const char *url, HttpBuffer *buf)
{
    return do_get(url, buf);
}

void http_buffer_free(HttpBuffer *buf)
{
    if (buf) {
        free(buf->data);
        buf->data = NULL;
        buf->size = 0;
        buf->capacity = 0;
    }
}

const char *http_last_error(void)
{
    return g_last_error;
}