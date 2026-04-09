#include "http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_BUF_SIZE 4096
#define USER_AGENT       "RouteASCII/1.0 (TUI Map Viewer)"

static CURL *g_curl = NULL;

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
    }
}

void http_cleanup(void)
{
    if (g_curl) {
        curl_easy_cleanup(g_curl);
        g_curl = NULL;
    }
    curl_global_cleanup();
}

static int do_get(const char *url, HttpBuffer *buf)
{
    if (!g_curl || !url || !buf) return -1;

    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, buf);

    CURLcode res = curl_easy_perform(g_curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[HTTP] Error: %s (url: %s)\n", curl_easy_strerror(res), url);
        http_buffer_free(buf);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "[HTTP] Status %ld for %s\n", http_code, url);
        http_buffer_free(buf);
        return -1;
    }

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