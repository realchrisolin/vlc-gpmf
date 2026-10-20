/*
 * Raster tiles for the GNSS inset.
 *
 * OpenStreetMap, Mapbox Streets, and Google roadmap. The filter thread
 * paints cached tiles. One worker fetches them with VLC's TLS stack so a
 * Google session can be created with POST and Cache-Control max-age is
 * visible. Playback does not wait on the network.
 *
 * stb_image.h (public domain / MIT, nothings/stb) decodes the PNG and JPEG.
 *
 * Copyright (C) 2026 Chris Olin
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hud_tiles.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
#endif
#include <vlc_common.h>
#include <vlc_threads.h>
#include <vlc_tls.h>
#include <vlc_url.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
/* One decoder thread. MinGW TLS would link libgcc's emutls and winpthread. */
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define TILE_PX 256
#define TILE_BYTES (TILE_PX * TILE_PX * 4)
#define CACHE_N 256
#define QUEUE_N 16
/* A readable tile is about 256 screen pixels. The inset stays well under
 * eight tiles on a side. A full-frame map on a 4K picture needs up to
 * sixteen, and the cache holds that view. */
#define TILE_WIN 16
#define VIS_N (TILE_WIN * TILE_WIN)
#define SCALE_N 32
#define BODY_MAX (512 * 1024)
#define TOKEN_N 512
#define CREDIT_N 160
#define UA "vlc-gpmf/1.0 (personal GNSS overlay)"

typedef struct {
    int source;
    int z, x, y;
    int fail;
    vlc_tick_t expires;
    uint64_t used;
    uint8_t *rgba;
} tile_slot;

typedef struct {
    int z, x, y;
    int ux;
    int gen;
    int source;
    char token[TOKEN_N];
    char session[TOKEN_N];
} tile_job;

typedef struct {
    int x, y, ux;
} vis_tile;

struct hud_tiles {
    vlc_object_t *obj;
    vlc_tls_creds_t *creds;
    vlc_thread_t thread;
    int started;
    vlc_mutex_t lock;
    vlc_cond_t wake;
    int stop;

    int source;
    int zoom;
    int gen;
    char token[TOKEN_N];
    char credit[CREDIT_N];
    char session[TOKEN_N];
    time_t session_exp;
    time_t session_retry;
    int session_fail_logged;
    time_t attr_retry;
    int attr_dirty;
    int attr_z, attr_x, attr_y;
    int logged_ok;
    int logged_fail;
    int warned_token;
    int token_rejected;

    double lat, lon;
    int view_w, view_h;
    int span; /* video pixels covered by one 256px tile */
    double north, south, east, west;

    tile_slot slot[CACHE_N];
    uint64_t clock;
    uint64_t stored; /* cache_store and cache_clear only */
    tile_job queue[QUEUE_N];
    int qn;
    int busy;
    int busy_z, busy_x, busy_y, busy_gen;

    /* One enlarged copy of a tile. Paint copies it. The blend runs once. */
    struct {
        const uint8_t *src;
        int span;
        uint64_t used;
        uint8_t *rgba;
    } scaled[SCALE_N];
    uint64_t scale_gen;
};

typedef struct {
    int status;
    int max_age;
    int saw_age;
    int no_store;
    char *location;
    uint8_t *body;
    size_t len;
} http_msg;

typedef struct {
    vlc_tls_t *tls;
    http_msg *msg;
} http_hold;

static void *worker(void *opaque);

static const char *source_name(int source)
{
    if (source == HUD_TILE_MAPBOX)
        return "mapbox";
    if (source == HUD_TILE_GOOGLE)
        return "google";
    return "osm";
}

static int clamp_zoom(int zoom)
{
    if (zoom < 2)
        return 2;
    if (zoom > 18)
        return 18;
    return zoom;
}

static void http_msg_init(http_msg *m)
{
    memset(m, 0, sizeof *m);
    m->max_age = -1;
}

static void http_msg_clear(http_msg *m)
{
    free(m->location);
    free(m->body);
    http_msg_init(m);
}

static void hold_abort(void *opaque)
{
    http_hold *hold = opaque;
    if (hold->tls) {
        vlc_tls_Close(hold->tls);
        hold->tls = NULL;
    }
    if (hold->msg) {
        free(hold->msg->body);
        free(hold->msg->location);
        hold->msg->body = NULL;
        hold->msg->location = NULL;
    }
}

static const char *find_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;
    if (n == 0)
        return hay;
    for (; *hay; hay++) {
        for (i = 0; i < n; i++) {
            unsigned char a = (unsigned char)hay[i];
            unsigned char b = (unsigned char)needle[i];
            if (a == 0)
                return NULL;
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (i == n)
            return hay;
    }
    return NULL;
}

static int append_text(char *dst, size_t n, size_t *used, const char *s)
{
    size_t len = strlen(s);
    if (*used + len >= n)
        return -1;
    memcpy(dst + *used, s, len);
    *used += len;
    dst[*used] = 0;
    return 0;
}

static int append_enc(char *dst, size_t n, size_t *used, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (plain) {
            if (*used + 1 >= n)
                return -1;
            dst[(*used)++] = (char)c;
        } else {
            if (*used + 3 >= n)
                return -1;
            dst[(*used)++] = '%';
            dst[(*used)++] = hex[c >> 4];
            dst[(*used)++] = hex[c & 15];
        }
        dst[*used] = 0;
    }
    return 0;
}

static void note_cache(http_msg *msg, const char *value)
{
    const char *p = value;
    if (find_ci(value, "no-store") || find_ci(value, "no-cache"))
        msg->no_store = 1;
    while ((p = find_ci(p, "max-age")) != NULL) {
        /* s-maxage is for shared caches. This cache is private. */
        int shared = p > value + 1 && p[-1] == '-' && (p[-2] == 's' || p[-2] == 'S');
        if (!shared) {
            const char *eq = strchr(p, '=');
            if (eq) {
                msg->max_age = atoi(eq + 1);
                msg->saw_age = 1;
            }
        }
        p += 7;
    }
}

static int read_exact(vlc_tls_t *tls, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = vlc_tls_Read(tls, buf + got, len - got, true);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static int read_chunked(vlc_tls_t *tls, http_msg *msg)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    for (;;) {
        char *line = vlc_tls_GetLine(tls);
        unsigned long size;
        char *end = NULL;
        if (!line)
            goto fail;
        size = strtoul(line, &end, 16);
        free(line);
        if (size == 0) {
            for (;;) {
                char *trailer = vlc_tls_GetLine(tls);
                int blank = !trailer || trailer[0] == 0;
                free(trailer);
                if (blank)
                    break;
            }
            break;
        }
        if (size > BODY_MAX || len + size > BODY_MAX)
            goto fail;
        if (len + size > len) {
            uint8_t *next = realloc(buf, len + size);
            if (!next)
                goto fail;
            buf = next;
        }
        if (read_exact(tls, buf + len, size))
            goto fail;
        len += size;
        {
            char crlf[2];
            if (read_exact(tls, (uint8_t *)crlf, 2))
                goto fail;
        }
    }
    msg->body = buf;
    msg->len = len;
    return 0;
fail:
    free(buf);
    return -1;
}

static int read_to_end(vlc_tls_t *tls, http_msg *msg, size_t known)
{
    uint8_t *buf;
    size_t len = 0;
    size_t cap;
    if (known > BODY_MAX)
        return -1;
    cap = known ? known : 8192;
    buf = malloc(cap ? cap : 1);
    if (!buf)
        return -1;
    if (known) {
        if (read_exact(tls, buf, known)) {
            free(buf);
            return -1;
        }
        msg->body = buf;
        msg->len = known;
        return 0;
    }
    while (len < BODY_MAX) {
        size_t room;
        ssize_t n;
        if (cap - len < 2048) {
            size_t ncap = cap * 2;
            uint8_t *next;
            if (ncap > BODY_MAX)
                ncap = BODY_MAX;
            if (ncap <= cap)
                break;
            next = realloc(buf, ncap);
            if (!next) {
                free(buf);
                return -1;
            }
            buf = next;
            cap = ncap;
        }
        room = cap - len;
        n = vlc_tls_Read(tls, buf + len, room, false);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    if (len >= BODY_MAX) {
        uint8_t extra;
        ssize_t n = vlc_tls_Read(tls, &extra, 1, false);
        if (n > 0) {
            free(buf);
            return -1;
        }
    }
    msg->body = buf;
    msg->len = len;
    return 0;
}

static int header_value(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char a = (unsigned char)line[i];
        unsigned char b = (unsigned char)name[i];
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    if (line[n] != ':')
        return 0;
    *value = line + n + 1;
    while (**value == ' ' || **value == '\t')
        (*value)++;
    return 1;
}

/* One HTTP/1.1 request. Redirects are handled by the caller. */
static int http_once(hud_tiles *t, const char *method, const char *url,
                     const char *ctype, const char *req_body, http_msg *msg)
{
    vlc_url_t parts;
    http_hold hold;
    char *req = NULL;
    char hosthdr[256];
    const char *path;
    const char *query;
    int https;
    unsigned port;
    int ok = 0;
    int known = 0;
    int have_len = 0;
    int chunked = 0;
    size_t body_len = req_body ? strlen(req_body) : 0;

    http_msg_init(msg);
    memset(&hold, 0, sizeof hold);
    hold.msg = msg;
    vlc_UrlParse(&parts, url);
    https = parts.psz_protocol && strcmp(parts.psz_protocol, "https") == 0;
    if (!https && !(parts.psz_protocol && strcmp(parts.psz_protocol, "http") == 0))
        goto out;
    if (!parts.psz_host || !parts.psz_host[0])
        goto out;
    if (!t->creds && https)
        goto out;
    port = parts.i_port ? parts.i_port : (https ? 443 : 80);
    if (strchr(parts.psz_host, ':'))
        snprintf(hosthdr, sizeof hosthdr, "[%s]", parts.psz_host);
    else
        snprintf(hosthdr, sizeof hosthdr, "%s", parts.psz_host);
    if (!((https && port == 443) || (!https && port == 80))) {
        size_t used = strlen(hosthdr);
        snprintf(hosthdr + used, sizeof hosthdr - used, ":%u", port);
    }
    path = parts.psz_path && parts.psz_path[0] ? parts.psz_path : "/";
    query = parts.psz_option;
    req = malloc(8192 + body_len);
    if (!req)
        goto out;
    {
        int n = snprintf(req, 8192,
                         "%s %s%s%s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: %s\r\n"
                         "Accept: image/png, image/jpeg, application/json\r\n"
                         "Connection: close\r\n",
                         method, path, query ? "?" : "", query ? query : "",
                         hosthdr, UA);
        if (n < 0 || n >= 8192)
            goto out;
        if (req_body) {
            int m = snprintf(req + n, 8192 - (size_t)n,
                             "Content-Type: %s\r\nContent-Length: %u\r\n\r\n",
                             ctype ? ctype : "application/octet-stream",
                             (unsigned)body_len);
            if (m < 0 || n + m >= 8192)
                goto out;
            memcpy(req + n + m, req_body, body_len);
            n += m + (int)body_len;
        } else {
            if (n + 2 >= 8192)
                goto out;
            req[n++] = '\r';
            req[n++] = '\n';
        }
        vlc_cleanup_push(hold_abort, &hold);
        if (https)
            hold.tls = vlc_tls_SocketOpenTLS(t->creds, parts.psz_host, port,
                                             "https", NULL, NULL);
        else
            hold.tls = vlc_tls_SocketOpenTCP(t->obj, parts.psz_host, port);
        if (hold.tls) {
            ssize_t wrote = vlc_tls_Write(hold.tls, req, (size_t)n);
            if (wrote == (ssize_t)n) {
                char *status = vlc_tls_GetLine(hold.tls);
                if (status) {
                    char *sp = strchr(status, ' ');
                    if (sp)
                        msg->status = atoi(sp + 1);
                    free(status);
                }
                if (msg->status > 0) {
                    for (;;) {
                        char *line = vlc_tls_GetLine(hold.tls);
                        const char *value = NULL;
                        if (!line)
                            break;
                        if (line[0] == 0) {
                            free(line);
                            ok = 1;
                            break;
                        }
                        if (header_value(line, "cache-control", &value))
                            note_cache(msg, value);
                        else if (header_value(line, "content-length", &value)) {
                            known = atoi(value);
                            have_len = 1;
                        }
                        else if (header_value(line, "transfer-encoding", &value)) {
                            if (find_ci(value, "chunked"))
                                chunked = 1;
                        } else if (header_value(line, "location", &value)) {
                            free(msg->location);
                            msg->location = strdup(value);
                        }
                        free(line);
                    }
                }
                if (ok) {
                    if (chunked)
                        ok = read_chunked(hold.tls, msg) == 0;
                    else if (have_len)
                        ok = known > 0 && known <= BODY_MAX &&
                             read_to_end(hold.tls, msg, (size_t)known) == 0;
                    else
                        ok = read_to_end(hold.tls, msg, 0) == 0;
                }
            }
            vlc_tls_Close(hold.tls);
            hold.tls = NULL;
        }
        vlc_cleanup_pop();
    }
out:
    free(req);
    vlc_UrlClean(&parts);
    if (!ok)
        http_msg_clear(msg);
    return ok ? 0 : -1;
}

static char *redirect_url(const char *current, const char *location)
{
    vlc_url_t parts;
    char *next = NULL;
    if (!location || !location[0])
        return NULL;
    if (strncmp(location, "https://", 8) == 0 || strncmp(location, "http://", 7) == 0)
        return strdup(location);
    if (location[0] != '/')
        return NULL;
    vlc_UrlParse(&parts, current);
    if (parts.psz_protocol && parts.psz_host) {
        unsigned port = parts.i_port;
        int https = strcmp(parts.psz_protocol, "https") == 0;
        next = malloc(strlen(location) + strlen(parts.psz_host) + 32);
        if (next) {
            if (port == 0 || (https && port == 443) || (!https && port == 80))
                snprintf(next, strlen(location) + strlen(parts.psz_host) + 32,
                         "%s://%s%s", parts.psz_protocol, parts.psz_host, location);
            else
                snprintf(next, strlen(location) + strlen(parts.psz_host) + 32,
                         "%s://%s:%u%s", parts.psz_protocol, parts.psz_host, port, location);
        }
    }
    vlc_UrlClean(&parts);
    return next;
}

static int http_exchange(hud_tiles *t, const char *method, const char *url,
                         const char *ctype, const char *req_body, http_msg *msg)
{
    char *current = strdup(url);
    char meth[8];
    const char *body = req_body;
    int hop;
    if (!current)
        return -1;
    snprintf(meth, sizeof meth, "%s", method);
    for (hop = 0; hop < 4; hop++) {
        char *loc;
        char *next;
        int switch_get;
        if (http_once(t, meth, current, ctype, body, msg)) {
            free(current);
            return -1;
        }
        if (msg->status < 300 || msg->status >= 400 || !msg->location)
            break;
        loc = strdup(msg->location);
        switch_get = msg->status == 301 || msg->status == 302 || msg->status == 303;
        http_msg_clear(msg);
        next = redirect_url(current, loc);
        free(loc);
        free(current);
        current = next;
        if (!current)
            return -1;
        if (switch_get) {
            snprintf(meth, sizeof meth, "GET");
            body = NULL;
        }
    }
    free(current);
    return msg->status > 0 ? 0 : -1;
}

static int json_string(const char *json, const char *key, char *out, size_t outn)
{
    char pat[48];
    const char *p;
    size_t i = 0;
    if (!json || outn == 0)
        return -1;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != ':')
        return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"')
        return -1;
    p++;
    while (*p && *p != '"' && i + 1 < outn) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u') {
                p++;
                continue;
            }
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0 ? 0 : -1;
}

static void fold_ascii(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    if (n == 0)
        return;
    while (src && *src && i + 1 < n) {
        unsigned char c = (unsigned char)*src;
        if (c == 0xC2 && (unsigned char)src[1] == 0xA9) {
            if (i + 3 >= n)
                break;
            dst[i++] = '(';
            dst[i++] = 'c';
            dst[i++] = ')';
            src += 2;
            continue;
        }
        if (c < 32 || c > 126) {
            src++;
            continue;
        }
        dst[i++] = (char)c;
        src++;
    }
    dst[i] = 0;
}

static void credit_for(hud_tiles *t, int source, const char *google_copy)
{
    if (source == HUD_TILE_MAPBOX)
        snprintf(t->credit, sizeof t->credit, "(c) Mapbox  (c) OpenStreetMap");
    else if (source == HUD_TILE_GOOGLE) {
        char plain[96];
        fold_ascii(plain, sizeof plain, google_copy ? google_copy : "");
        if (plain[0])
            snprintf(t->credit, sizeof t->credit, "Google Maps  %s", plain);
        else
            snprintf(t->credit, sizeof t->credit, "Google Maps");
    } else
        snprintf(t->credit, sizeof t->credit, "(c) OpenStreetMap");
}

static int ttl_seconds(int source, const http_msg *msg)
{
    if (msg->no_store || (msg->saw_age && msg->max_age <= 0))
        return 2;
    if (msg->saw_age && msg->max_age > 0) {
        if (msg->max_age > 7 * 24 * 3600)
            return 7 * 24 * 3600;
        return msg->max_age;
    }
    if (source == HUD_TILE_MAPBOX)
        return 12 * 3600;
    if (source == HUD_TILE_GOOGLE)
        return 60;
    return 3600;
}

static char *url_osm(int z, int x, int y)
{
    char *url = malloc(80);
    if (!url)
        return NULL;
    snprintf(url, 80, "https://tile.openstreetmap.org/%d/%d/%d.png", z, x, y);
    return url;
}

static char *url_mapbox(int z, int x, int y, const char *token)
{
    char *url = malloc(768);
    size_t used;
    int n;
    if (!url)
        return NULL;
    n = snprintf(url, 768,
                 "https://api.mapbox.com/styles/v1/mapbox/streets-v12/tiles/256/%d/%d/%d?access_token=",
                 z, x, y);
    if (n < 0 || n >= 768) {
        free(url);
        return NULL;
    }
    used = (size_t)n;
    if (append_enc(url, 768, &used, token)) {
        free(url);
        return NULL;
    }
    return url;
}

static char *url_google_tile(int z, int x, int y, const char *session, const char *key)
{
    char *url = malloc(1280);
    size_t used;
    int n;
    if (!url)
        return NULL;
    n = snprintf(url, 1280, "https://tile.googleapis.com/v1/2dtiles/%d/%d/%d?session=", z, x, y);
    if (n < 0 || n >= 1280) {
        free(url);
        return NULL;
    }
    used = (size_t)n;
    if (append_enc(url, 1280, &used, session) ||
        append_text(url, 1280, &used, "&key=") ||
        append_enc(url, 1280, &used, key)) {
        free(url);
        return NULL;
    }
    return url;
}

static char *url_google_session(const char *key)
{
    char *url = malloc(640);
    size_t used;
    if (!url)
        return NULL;
    used = 0;
    url[0] = 0;
    if (append_text(url, 640, &used, "https://tile.googleapis.com/v1/createSession?key=") ||
        append_enc(url, 640, &used, key)) {
        free(url);
        return NULL;
    }
    return url;
}

static char *url_google_viewport(const char *session, const char *key, int zoom,
                                 double north, double south, double east, double west)
{
    char *url = malloc(1400);
    size_t used;
    char tail[160];
    if (!url)
        return NULL;
    used = 0;
    url[0] = 0;
    snprintf(tail, sizeof tail, "&zoom=%d&north=%.5f&south=%.5f&east=%.5f&west=%.5f",
             zoom, north, south, east, west);
    if (append_text(url, 1400, &used, "https://tile.googleapis.com/tile/v1/viewport?session=") ||
        append_enc(url, 1400, &used, session) ||
        append_text(url, 1400, &used, "&key=") ||
        append_enc(url, 1400, &used, key) ||
        append_text(url, 1400, &used, tail)) {
        free(url);
        return NULL;
    }
    return url;
}

static uint8_t *fit_tile(uint8_t *src, int w, int h)
{
    uint8_t *dst;
    int y, x;
    if (w == TILE_PX && h == TILE_PX)
        return src;
    if (w < 1 || h < 1 || w > 2048 || h > 2048) {
        stbi_image_free(src);
        return NULL;
    }
    dst = malloc(TILE_BYTES);
    if (!dst) {
        stbi_image_free(src);
        return NULL;
    }
    for (y = 0; y < TILE_PX; y++) {
        int sy = y * h / TILE_PX;
        for (x = 0; x < TILE_PX; x++) {
            int sx = x * w / TILE_PX;
            memcpy(dst + ((size_t)y * TILE_PX + (size_t)x) * 4,
                   src + ((size_t)sy * (size_t)w + (size_t)sx) * 4, 4);
        }
    }
    stbi_image_free(src);
    return dst;
}

static uint8_t *decode_tile(const http_msg *msg)
{
    int w = 0, h = 0, comp = 0;
    stbi_uc *px;
    if (!msg->body || msg->len < 8 || msg->len > BODY_MAX)
        return NULL;
    px = stbi_load_from_memory(msg->body, (int)msg->len, &w, &h, &comp, 4);
    if (!px)
        return NULL;
    return fit_tile(px, w, h);
}

static int slot_match(const tile_slot *s, int source, int z, int x, int y)
{
    return s->used && s->source == source && s->z == z && s->x == x && s->y == y;
}

static int slot_live(const tile_slot *s, vlc_tick_t now)
{
    if (!s->used || s->expires <= now)
        return 0;
    if (s->fail)
        return 1;
    return s->rgba != NULL;
}

static int slot_pick(hud_tiles *t, int source, int z, int x, int y)
{
    int i;
    int lru = 0;
    vlc_tick_t now = mdate();
    for (i = 0; i < CACHE_N; i++) {
        if (slot_match(&t->slot[i], source, z, x, y))
            return i;
    }
    for (i = 0; i < CACHE_N; i++) {
        if (!t->slot[i].used || !slot_live(&t->slot[i], now))
            return i;
        if (t->slot[i].used < t->slot[lru].used)
            lru = i;
    }
    return lru;
}

static void drop_scaled(hud_tiles *t, const uint8_t *src)
{
    int i;
    for (i = 0; i < SCALE_N; i++) {
        if (!t->scaled[i].rgba)
            continue;
        if (src && t->scaled[i].src != src)
            continue;
        free(t->scaled[i].rgba);
        memset(&t->scaled[i], 0, sizeof t->scaled[i]);
    }
}

/* Blend a 256px tile up to span pixels. The result is cached and copied. */
static uint8_t *scale_tile(const uint8_t *src, int span)
{
    uint8_t *dst;
    int y, x;
    if (!src || span < 1 || span > 2048)
        return NULL;
    dst = (uint8_t *)malloc((size_t)span * (size_t)span * 4);
    if (!dst)
        return NULL;
    for (y = 0; y < span; y++) {
        double sy = ((double)y + 0.5) * 256.0 / (double)span - 0.5;
        int y0, y1;
        double fy;
        if (sy < 0)
            sy = 0;
        if (sy > TILE_PX - 1)
            sy = TILE_PX - 1;
        y0 = (int)sy;
        fy = sy - (double)y0;
        y1 = y0 < TILE_PX - 1 ? y0 + 1 : y0;
        for (x = 0; x < span; x++) {
            double sx = ((double)x + 0.5) * 256.0 / (double)span - 0.5;
            int x0, x1, c;
            double fx, w00, w10, w01, w11;
            const uint8_t *p00, *p10, *p01, *p11;
            uint8_t *d;
            if (sx < 0)
                sx = 0;
            if (sx > TILE_PX - 1)
                sx = TILE_PX - 1;
            x0 = (int)sx;
            fx = sx - (double)x0;
            x1 = x0 < TILE_PX - 1 ? x0 + 1 : x0;
            p00 = src + ((size_t)y0 * TILE_PX + (size_t)x0) * 4;
            p10 = src + ((size_t)y0 * TILE_PX + (size_t)x1) * 4;
            p01 = src + ((size_t)y1 * TILE_PX + (size_t)x0) * 4;
            p11 = src + ((size_t)y1 * TILE_PX + (size_t)x1) * 4;
            w11 = fx * fy;
            w10 = fx * (1.0 - fy);
            w01 = (1.0 - fx) * fy;
            w00 = (1.0 - fx) * (1.0 - fy);
            d = dst + ((size_t)y * (size_t)span + (size_t)x) * 4;
            for (c = 0; c < 4; c++)
                d[c] = (uint8_t)(p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11 + 0.5);
        }
    }
    return dst;
}

static uint8_t *scaled_rgba(hud_tiles *t, const uint8_t *src, int span)
{
    int i, slot;
    uint8_t *dst;
    if (!src || span <= 0)
        return NULL;
    slot = -1;
    for (i = 0; i < SCALE_N; i++) {
        if (t->scaled[i].rgba && t->scaled[i].src == src && t->scaled[i].span == span) {
            t->scaled[i].used = t->scale_gen;
            return t->scaled[i].rgba;
        }
        if (!t->scaled[i].rgba) {
            if (slot < 0)
                slot = i;
            continue;
        }
        if (t->scaled[i].used == t->scale_gen)
            continue;
        if (slot < 0 || !t->scaled[slot].rgba || t->scaled[i].used < t->scaled[slot].used)
            slot = i;
    }
    if (slot < 0)
        return NULL;
    dst = scale_tile(src, span);
    if (!dst)
        return NULL;
    free(t->scaled[slot].rgba);
    t->scaled[slot].src = src;
    t->scaled[slot].span = span;
    t->scaled[slot].rgba = dst;
    t->scaled[slot].used = t->scale_gen;
    return dst;
}

static void cache_store(hud_tiles *t, int source, int z, int x, int y,
                        uint8_t *rgba, int fail, int ttl)
{
    int i = slot_pick(t, source, z, x, y);
    tile_slot *s = &t->slot[i];
    if (s->rgba && s->rgba != rgba) {
        drop_scaled(t, s->rgba);
        free(s->rgba);
    }
    memset(s, 0, sizeof *s);
    s->source = source;
    s->z = z;
    s->x = x;
    s->y = y;
    s->fail = fail;
    s->rgba = fail ? NULL : rgba;
    s->expires = mdate() + (vlc_tick_t)ttl * CLOCK_FREQ;
    s->used = ++t->clock;
    t->stored++;
    if (fail)
        free(rgba);
}

static void cache_clear(hud_tiles *t)
{
    int i;
    drop_scaled(t, NULL);
    for (i = 0; i < CACHE_N; i++) {
        free(t->slot[i].rgba);
        memset(&t->slot[i], 0, sizeof t->slot[i]);
    }
    t->qn = 0;
    t->busy = 0;
    t->stored++;
}

static int wrap_x(int x, int n)
{
    if (n <= 0)
        return 0;
    x %= n;
    if (x < 0)
        x += n;
    return x;
}

static double lat_of_tile(double y_tile, int n)
{
    double yn = y_tile / (double)n;
    double merc = (1.0 - 2.0 * yn) * 3.141592653589793;
    return atan(sinh(merc)) * 180.0 / 3.141592653589793;
}

static void view_tiles(double lat, double lon, int zoom, double view_w, double view_h,
                       vis_tile *vis, int *nvis, double *left, double *top,
                       double *north, double *south, double *east, double *west,
                       int *cx, int *cy)
{
    double mx, my;
    int n, x0, y0, x1, y1, tx, ty, count;
    hud_map_mercator(lat, lon, zoom, &mx, &my);
    n = 1 << zoom;
    *left = mx * 256.0 - view_w * 0.5;
    *top = my * 256.0 - view_h * 0.5;
    x0 = (int)floor(*left / 256.0);
    y0 = (int)floor(*top / 256.0);
    x1 = (int)floor((*left + view_w - 1e-6) / 256.0);
    y1 = (int)floor((*top + view_h - 1e-6) / 256.0);
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    if (x1 - x0 + 1 > TILE_WIN) {
        int mid = (x0 + x1) / 2;
        x0 = mid - TILE_WIN / 2;
        x1 = x0 + TILE_WIN - 1;
    }
    if (y1 - y0 + 1 > TILE_WIN) {
        int mid = (y0 + y1) / 2;
        y0 = mid - TILE_WIN / 2;
        y1 = y0 + TILE_WIN - 1;
    }
    count = 0;
    for (ty = y0; ty <= y1; ty++) {
        if (ty < 0 || ty >= n)
            continue;
        for (tx = x0; tx <= x1; tx++) {
            if (count >= VIS_N)
                break;
            vis[count].ux = tx;
            vis[count].x = wrap_x(tx, n);
            vis[count].y = ty;
            count++;
        }
    }
    *nvis = count;
    *cx = wrap_x((int)floor(mx), n);
    *cy = (int)floor(my);
    if (*cy < 0)
        *cy = 0;
    if (*cy >= n)
        *cy = n - 1;
    *north = lat_of_tile(*top / 256.0, n);
    *south = lat_of_tile((*top + view_h) / 256.0, n);
    *west = (*left / 256.0) / (double)n * 360.0 - 180.0;
    *east = ((*left + view_w) / 256.0) / (double)n * 360.0 - 180.0;
    if (*north > 85.0)
        *north = 85.0;
    if (*south < -85.0)
        *south = -85.0;
    if (*north < *south)
        *north = *south;
    if (*west < -180.0)
        *west = -180.0;
    if (*east > 180.0)
        *east = 180.0;
}

static int tile_dist2(const vis_tile *v, int cx, int cy)
{
    int dx = v->x - cx;
    int dy = v->y - cy;
    return dx * dx + dy * dy;
}

/* Nearest the fix first, so the arrow's neighborhood arrives before the corners. */
static void sort_vis(vis_tile *vis, int n, int cx, int cy)
{
    int i, j;
    for (i = 1; i < n; i++) {
        vis_tile v = vis[i];
        int d = tile_dist2(&v, cx, cy);
        j = i;
        while (j > 0 && tile_dist2(&vis[j - 1], cx, cy) > d) {
            vis[j] = vis[j - 1];
            j--;
        }
        vis[j] = v;
    }
}

static int queued(const hud_tiles *t, int x, int y)
{
    int i;
    for (i = 0; i < t->qn; i++) {
        if (t->queue[i].x == x && t->queue[i].y == y && t->queue[i].z == t->zoom)
            return 1;
    }
    return 0;
}

static int session_usable(const hud_tiles *t, time_t now)
{
    return t->session[0] && now < t->session_exp && now != (time_t)-1;
}

static char *tile_url(const tile_job *job)
{
    if (job->source == HUD_TILE_MAPBOX)
        return url_mapbox(job->z, job->x, job->y, job->token);
    if (job->source == HUD_TILE_GOOGLE)
        return url_google_tile(job->z, job->x, job->y, job->session, job->token);
    return url_osm(job->z, job->x, job->y);
}

static void fetch_tile(hud_tiles *t, const tile_job *job)
{
    char *url;
    http_msg msg;
    uint8_t *rgba = NULL;
    int status = 0;
    int ttl = 30;
    int fail = 1;
    int do_ok = 0;
    int do_fail = 0;
    int canc;

    url = tile_url(job);
    http_msg_init(&msg);
    if (url && http_exchange(t, "GET", url, NULL, NULL, &msg) == 0)
        status = msg.status;
    free(url);
    if (status == 200)
        rgba = decode_tile(&msg);
    if (rgba) {
        ttl = ttl_seconds(job->source, &msg);
        fail = 0;
    }
    http_msg_clear(&msg);

    canc = vlc_savecancel();
    vlc_mutex_lock(&t->lock);
    t->busy = 0;
    if (!t->stop && job->gen == t->gen) {
        cache_store(t, job->source, job->z, job->x, job->y, rgba, fail, ttl);
        rgba = NULL;
        if (!fail && !t->logged_ok) {
            t->logged_ok = 1;
            do_ok = 1;
        } else if (fail && !t->logged_fail) {
            t->logged_fail = 1;
            do_fail = 1;
        }
    }
    vlc_mutex_unlock(&t->lock);
    free(rgba);
    vlc_restorecancel(canc);
    if (do_ok)
        msg_Info(t->obj, "gpmf map %s tile %d/%d/%d",
                 source_name(job->source), job->z, job->x, job->y);
    if (do_fail)
        msg_Warn(t->obj, "gpmf map %s tile %d/%d/%d status %d",
                 source_name(job->source), job->z, job->x, job->y, status);
}

static void fetch_session(hud_tiles *t, const char *key, int gen)
{
    static const char body[] =
        "{\"mapType\":\"roadmap\",\"language\":\"en-US\",\"region\":\"US\"}";
    char *url = url_google_session(key);
    http_msg msg;
    char session[TOKEN_N];
    char expiry[32];
    long long exp = 0;
    int ok = 0;
    int status = 0;
    int canc;
    int do_log = 0;
    int do_fail = 0;

    session[0] = 0;
    expiry[0] = 0;
    http_msg_init(&msg);
    if (url && http_exchange(t, "POST", url, "application/json", body, &msg) == 0) {
        status = msg.status;
        if (status == 200 && msg.body) {
            char *text = malloc(msg.len + 1);
            if (text) {
                memcpy(text, msg.body, msg.len);
                text[msg.len] = 0;
                if (json_string(text, "session", session, sizeof session) == 0) {
                    if (json_string(text, "expiry", expiry, sizeof expiry) == 0)
                        exp = strtoll(expiry, NULL, 10);
                    else {
                        const char *e = strstr(text, "\"expiry\"");
                        if (e && (e = strchr(e, ':')))
                            exp = strtoll(e + 1, NULL, 10);
                    }
                }
                free(text);
            }
        }
    }
    free(url);
    ok = session[0] && exp > 0;
    http_msg_clear(&msg);

    canc = vlc_savecancel();
    vlc_mutex_lock(&t->lock);
    if (!t->stop && gen == t->gen) {
        if (ok) {
            snprintf(t->session, sizeof t->session, "%s", session);
            t->session_exp = (time_t)exp;
            t->session_retry = 0;
            t->session_fail_logged = 0;
            t->attr_dirty = 1;
            do_log = 1;
        } else {
            t->session_retry = time(NULL) + 30;
            if (!t->session_fail_logged) {
                t->session_fail_logged = 1;
                do_fail = 1;
            }
        }
    }
    vlc_cond_signal(&t->wake);
    vlc_mutex_unlock(&t->lock);
    vlc_restorecancel(canc);
    if (do_log)
        msg_Info(t->obj, "gpmf map google session ready");
    if (do_fail)
        msg_Warn(t->obj, "gpmf map google session status %d", status);
}

static void fetch_viewport(hud_tiles *t, const char *session, const char *key,
                           int zoom, double north, double south, double east, double west,
                           int gen)
{
    char *url = url_google_viewport(session, key, zoom, north, south, east, west);
    http_msg msg;
    char copy[128];
    int ok = 0;
    int canc;

    copy[0] = 0;
    http_msg_init(&msg);
    if (url && http_exchange(t, "GET", url, NULL, NULL, &msg) == 0 &&
        msg.status == 200 && msg.body) {
        char *text = malloc(msg.len + 1);
        if (text) {
            memcpy(text, msg.body, msg.len);
            text[msg.len] = 0;
            if (json_string(text, "copyright", copy, sizeof copy) == 0)
                ok = 1;
            free(text);
        }
    }
    free(url);
    http_msg_clear(&msg);

    canc = vlc_savecancel();
    vlc_mutex_lock(&t->lock);
    if (!t->stop && gen == t->gen && t->source == HUD_TILE_GOOGLE) {
        if (ok)
            credit_for(t, HUD_TILE_GOOGLE, copy);
        else
            t->attr_retry = time(NULL) + 60;
        t->attr_dirty = ok ? 0 : 1;
    }
    vlc_mutex_unlock(&t->lock);
    vlc_restorecancel(canc);
}

static int google_needs_session(const hud_tiles *t, time_t now)
{
    int usable;
    int soon;
    if (t->source != HUD_TILE_GOOGLE || !t->token[0])
        return 0;
    usable = session_usable(t, now);
    soon = usable && now + 60 >= t->session_exp;
    if (usable && !soon)
        return 0;
    return now >= t->session_retry;
}

static void *worker(void *opaque)
{
    hud_tiles *t = opaque;
    vlc_mutex_lock(&t->lock);
    for (;;) {
        while (!t->stop && t->qn == 0 && !google_needs_session(t, time(NULL)) &&
               !(t->source == HUD_TILE_GOOGLE && t->token[0] && t->attr_dirty &&
                 session_usable(t, time(NULL)) && time(NULL) >= t->attr_retry)) {
            mutex_cleanup_push(&t->lock);
            vlc_cond_wait(&t->wake, &t->lock);
            vlc_cleanup_pop();
        }
        if (t->stop)
            break;
        if (google_needs_session(t, time(NULL))) {
            char key[TOKEN_N];
            int gen = t->gen;
            snprintf(key, sizeof key, "%s", t->token);
            vlc_mutex_unlock(&t->lock);
            fetch_session(t, key, gen);
            vlc_mutex_lock(&t->lock);
            continue;
        }
        if (t->source == HUD_TILE_GOOGLE && !session_usable(t, time(NULL))) {
            mutex_cleanup_push(&t->lock);
            vlc_cond_timedwait(&t->wake, &t->lock, mdate() + CLOCK_FREQ);
            vlc_cleanup_pop();
            continue;
        }
        if (t->qn > 0) {
            tile_job job = t->queue[0];
            memmove(&t->queue[0], &t->queue[1], (size_t)(t->qn - 1) * sizeof job);
            t->qn--;
            if (job.source == HUD_TILE_GOOGLE)
                snprintf(job.session, sizeof job.session, "%s", t->session);
            t->busy = 1;
            t->busy_z = job.z;
            t->busy_x = job.x;
            t->busy_y = job.y;
            t->busy_gen = job.gen;
            vlc_mutex_unlock(&t->lock);
            fetch_tile(t, &job);
            vlc_mutex_lock(&t->lock);
            continue;
        }
        if (t->source == HUD_TILE_GOOGLE && t->attr_dirty && t->session[0]) {
            char session[TOKEN_N];
            char key[TOKEN_N];
            double north = t->north, south = t->south, east = t->east, west = t->west;
            int zoom = t->zoom;
            int gen = t->gen;
            snprintf(session, sizeof session, "%s", t->session);
            snprintf(key, sizeof key, "%s", t->token);
            t->attr_dirty = 0;
            vlc_mutex_unlock(&t->lock);
            fetch_viewport(t, session, key, zoom, north, south, east, west, gen);
            vlc_mutex_lock(&t->lock);
            continue;
        }
        mutex_cleanup_push(&t->lock);
        vlc_cond_timedwait(&t->wake, &t->lock, mdate() + CLOCK_FREQ);
        vlc_cleanup_pop();
    }
    vlc_mutex_unlock(&t->lock);
    return NULL;
}

static void tiles_free(hud_tiles *t)
{
    int i;
    if (!t)
        return;
    drop_scaled(t, NULL);
    for (i = 0; i < CACHE_N; i++)
        free(t->slot[i].rgba);
    vlc_tls_Delete(t->creds);
    vlc_cond_destroy(&t->wake);
    vlc_mutex_destroy(&t->lock);
    free(t);
}

hud_tiles *hud_tiles_open(vlc_object_t *obj)
{
    hud_tiles *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->obj = obj;
    t->source = HUD_TILE_OSM;
    t->zoom = 0;
    snprintf(t->credit, sizeof t->credit, "(c) OpenStreetMap");
    vlc_mutex_init(&t->lock);
    vlc_cond_init(&t->wake);
    t->creds = vlc_tls_ClientCreate(obj);
    if (!t->creds)
        msg_Warn(obj, "gpmf map: TLS is not available");
    if (vlc_clone(&t->thread, worker, t, VLC_THREAD_PRIORITY_LOW)) {
        msg_Warn(obj, "gpmf map: tile thread did not start");
        tiles_free(t);
        return NULL;
    }
    t->started = 1;
    return t;
}

void hud_tiles_close(hud_tiles *t)
{
    if (!t)
        return;
    if (t->started) {
        vlc_mutex_lock(&t->lock);
        t->stop = 1;
        vlc_cond_signal(&t->wake);
        vlc_mutex_unlock(&t->lock);
        vlc_cancel(t->thread);
        vlc_join(t->thread, NULL);
    }
    tiles_free(t);
}

void hud_tiles_configure(hud_tiles *t, int source, const char *token, int zoom)
{
    int warn = 0;
    int changed = 0;
    int too_long = 0;
    if (!t)
        return;
    if (!token)
        token = "";
    too_long = token[0] && strlen(token) >= TOKEN_N;
    if (too_long)
        token = "";
    if (source != HUD_TILE_MAPBOX && source != HUD_TILE_GOOGLE)
        source = HUD_TILE_OSM;
    zoom = clamp_zoom(zoom);
    vlc_mutex_lock(&t->lock);
    if (source == t->source && zoom == t->zoom && t->token_rejected == too_long &&
        strcmp(token, t->token) == 0) {
        vlc_mutex_unlock(&t->lock);
        return;
    }
    changed = 1;
    if (source != t->source || t->token_rejected != too_long || strcmp(token, t->token) != 0) {
        t->source = source;
        t->token_rejected = too_long;
        snprintf(t->token, sizeof t->token, "%s", token);
        t->gen++;
        t->session[0] = 0;
        t->session_exp = 0;
        t->session_retry = 0;
        t->session_fail_logged = 0;
        t->attr_dirty = 1;
        t->attr_retry = 0;
        t->logged_ok = 0;
        t->logged_fail = 0;
        t->warned_token = 0;
        cache_clear(t);
        credit_for(t, source, NULL);
    }
    t->zoom = zoom;
    if (!t->warned_token && too_long) {
        t->warned_token = 1;
        warn = -1;
    } else if ((source == HUD_TILE_MAPBOX || source == HUD_TILE_GOOGLE) &&
               !t->token[0] && !t->warned_token) {
        t->warned_token = 1;
        warn = source;
    }
    vlc_cond_signal(&t->wake);
    vlc_mutex_unlock(&t->lock);
    if (changed)
        msg_Info(t->obj, "gpmf map source %s zoom %d", source_name(source), zoom);
    if (warn < 0)
        msg_Warn(t->obj, "gpmf map token is too long; the inset stays dark");
    else if (warn)
        msg_Warn(t->obj, "gpmf map %s has no token; the inset stays dark",
                 source_name(warn));
}

uint64_t hud_tiles_stored(hud_tiles *t)
{
    uint64_t n;
    if (!t)
        return 0;
    vlc_mutex_lock(&t->lock);
    n = t->stored;
    vlc_mutex_unlock(&t->lock);
    return n;
}

void hud_tiles_want(hud_tiles *t, double lat, double lon, int view_w, int view_h, int span)
{
    vis_tile vis[VIS_N];
    int nvis = 0;
    double left, top, north, south, east, west;
    double world_w, world_h;
    int cx = 0, cy = 0;
    int i;
    int added = 0;
    vlc_tick_t now;
    if (!t || view_w < 8 || view_h < 8)
        return;
    if (span < 64)
        span = TILE_PX;
    world_w = (double)view_w * (double)TILE_PX / (double)span;
    world_h = (double)view_h * (double)TILE_PX / (double)span;
    vlc_mutex_lock(&t->lock);
    if ((t->source == HUD_TILE_MAPBOX || t->source == HUD_TILE_GOOGLE) && !t->token[0]) {
        vlc_mutex_unlock(&t->lock);
        return;
    }
    view_tiles(lat, lon, t->zoom, world_w, world_h, vis, &nvis,
               &left, &top, &north, &south, &east, &west, &cx, &cy);
    sort_vis(vis, nvis, cx, cy);
    t->lat = lat;
    t->lon = lon;
    t->view_w = view_w;
    t->view_h = view_h;
    t->span = span;
    t->north = north;
    t->south = south;
    t->east = east;
    t->west = west;
    if (cx != t->attr_x || cy != t->attr_y || t->zoom != t->attr_z) {
        t->attr_x = cx;
        t->attr_y = cy;
        t->attr_z = t->zoom;
        t->attr_dirty = 1;
    }
    now = mdate();
    {
        int w = 0;
        for (i = 0; i < t->qn; i++) {
            int keep = 0;
            int k;
            if (t->queue[i].gen != t->gen || t->queue[i].z != t->zoom)
                continue;
            for (k = 0; k < nvis; k++) {
                if (vis[k].x == t->queue[i].x && vis[k].y == t->queue[i].y)
                    keep = 1;
            }
            if (keep)
                t->queue[w++] = t->queue[i];
        }
        t->qn = w;
    }
    for (i = 0; i < nvis && t->qn < QUEUE_N; i++) {
        int s;
        int live = 0;
        for (s = 0; s < CACHE_N; s++) {
            if (slot_match(&t->slot[s], t->source, t->zoom, vis[i].x, vis[i].y) &&
                slot_live(&t->slot[s], now))
                live = 1;
        }
        if (live || queued(t, vis[i].x, vis[i].y))
            continue;
        if (t->busy && t->busy_gen == t->gen && t->busy_z == t->zoom &&
            t->busy_x == vis[i].x && t->busy_y == vis[i].y)
            continue;
        memset(&t->queue[t->qn], 0, sizeof t->queue[t->qn]);
        t->queue[t->qn].z = t->zoom;
        t->queue[t->qn].x = vis[i].x;
        t->queue[t->qn].y = vis[i].y;
        t->queue[t->qn].ux = vis[i].ux;
        t->queue[t->qn].gen = t->gen;
        t->queue[t->qn].source = t->source;
        snprintf(t->queue[t->qn].token, TOKEN_N, "%s", t->token);
        t->qn++;
        added = 1;
    }
    if (added || t->attr_dirty)
        vlc_cond_signal(&t->wake);
    vlc_mutex_unlock(&t->lock);
    (void)left;
    (void)top;
}

/* Full-frame map: the box is every pixel, so the rounded-rect test is skipped. */
static void blit_copy_full(unsigned char *pixels, int pitch, int pix,
                           int dest_x, int dest_y, const uint8_t *rgba, int src_w,
                           int x0, int y0, int x1, int y1, int cover)
{
    int sy;
    for (sy = y0; sy < y1; sy++) {
        const uint8_t *s = rgba + ((size_t)sy * (size_t)src_w + (size_t)x0) * 4;
        unsigned char *p = pixels + (size_t)(dest_y + sy) * (size_t)pitch
            + (size_t)(dest_x + x0) * (size_t)pix;
        int n = x1 - x0;
        int sx;
        if (cover >= 250) {
            for (sx = 0; sx < n; sx++) {
                int a = s[3];
                if (a >= 250) {
                    p[0] = s[0];
                    p[1] = s[1];
                    p[2] = s[2];
                    p[3] = 255;
                } else if (a > 0) {
                    int ia = 255 - a;
                    p[0] = (unsigned char)((s[0] * a + p[0] * ia) / 255);
                    p[1] = (unsigned char)((s[1] * a + p[1] * ia) / 255);
                    p[2] = (unsigned char)((s[2] * a + p[2] * ia) / 255);
                    p[3] = 255;
                }
                s += 4;
                p += pix;
            }
        } else {
            for (sx = 0; sx < n; sx++) {
                int a = s[3];
                int out;
                if (a > 0) {
                    out = a * cover / 255;
                    if (out > 0) {
                        p[0] = s[0];
                        p[1] = s[1];
                        p[2] = s[2];
                        p[3] = (unsigned char)out;
                    }
                }
                s += 4;
                p += pix;
            }
        }
    }
}

static void blit_copy(unsigned char *pixels, int pitch, int pix,
                      const hud_map_box *box, int dest_x, int dest_y,
                      const uint8_t *rgba, int src_w, int copy_w, int copy_h,
                      int cover)
{
    int y0, y1, x0, x1, sy, sx;
    if (!rgba || src_w < 1 || copy_w < 1 || copy_h < 1)
        return;
    if (copy_w > src_w)
        copy_w = src_w;
    y0 = dest_y < 0 ? -dest_y : 0;
    x0 = dest_x < 0 ? -dest_x : 0;
    y1 = copy_h;
    x1 = copy_w;
    if (dest_y + y1 > box->h)
        y1 = box->h - dest_y;
    if (dest_x + x1 > box->w)
        x1 = box->w - dest_x;
    if (y0 >= y1 || x0 >= x1)
        return;
    if (box->full) {
        blit_copy_full(pixels, pitch, pix, dest_x, dest_y, rgba, src_w,
                       x0, y0, x1, y1, cover);
        return;
    }
    for (sy = y0; sy < y1; sy++) {
        int dy = dest_y + sy;
        for (sx = x0; sx < x1; sx++) {
            int dx = dest_x + sx;
            const uint8_t *s;
            unsigned char *p;
            int a;
            if (!hud_map_content(box, dx, dy))
                continue;
            s = rgba + ((size_t)sy * (size_t)src_w + (size_t)sx) * 4;
            a = s[3];
            if (a <= 0)
                continue;
            p = pixels + (size_t)dy * (size_t)pitch + (size_t)dx * (size_t)pix;
            if (cover < 250) {
                int out = a * cover / 255;
                if (out <= 0)
                    continue;
                p[0] = s[0];
                p[1] = s[1];
                p[2] = s[2];
                p[3] = (unsigned char)out;
                continue;
            }
            if (a >= 250) {
                p[0] = s[0];
                p[1] = s[1];
                p[2] = s[2];
                p[3] = 255;
            } else {
                int ia = 255 - a;
                p[0] = (unsigned char)((s[0] * a + p[0] * ia) / 255);
                p[1] = (unsigned char)((s[1] * a + p[1] * ia) / 255);
                p[2] = (unsigned char)((s[2] * a + p[2] * ia) / 255);
                p[3] = 255;
            }
        }
    }
}

static void blend_px(unsigned char *p, int r, int g, int b, int a, int cover)
{
    if (cover < 250) {
        int out = a * cover / 255;
        if (out <= 0)
            return;
        p[0] = (unsigned char)r;
        p[1] = (unsigned char)g;
        p[2] = (unsigned char)b;
        p[3] = (unsigned char)out;
        return;
    }
    if (a >= 250) {
        p[0] = (unsigned char)r;
        p[1] = (unsigned char)g;
        p[2] = (unsigned char)b;
        p[3] = 255;
    } else if (a > 0) {
        int ia = 255 - a;
        p[0] = (unsigned char)((r * a + p[0] * ia) / 255);
        p[1] = (unsigned char)((g * a + p[1] * ia) / 255);
        p[2] = (unsigned char)((b * a + p[2] * ia) / 255);
        p[3] = 255;
    }
}

/* Draw one tile across `span` video pixels, bilinear, so a label that was
 * authored at 256 screen pixels stays about that size after VLC scales
 * the picture down to the window. Neighboring tiles share edges. */
static void blit_span(unsigned char *pixels, int pitch, int pix,
                      const hud_map_box *box, int ux, int uy,
                      double left, double top, int span, const uint8_t *rgba,
                      int cover)
{
    double scale = (double)span / 256.0;
    int dest_x = (int)floor(((double)ux * 256.0 - left) * scale);
    int dest_y = (int)floor(((double)uy * 256.0 - top) * scale);
    int dest_r = (int)floor((((double)ux + 1.0) * 256.0 - left) * scale);
    int dest_b = (int)floor((((double)uy + 1.0) * 256.0 - top) * scale);
    int py;
    if (dest_r < dest_x)
        dest_r = dest_x;
    if (dest_b < dest_y)
        dest_b = dest_y;
    if (dest_x >= box->w || dest_y >= box->h || dest_r <= 0 || dest_b <= 0)
        return;
    for (py = dest_y; py < dest_b; py++) {
        double sy, fy;
        int y0, y1, px;
        if (py < 0 || py >= box->h)
            continue;
        sy = (top + ((double)py + 0.5) * 256.0 / (double)span) - (double)uy * 256.0 - 0.5;
        if (sy < 0)
            sy = 0;
        if (sy > TILE_PX - 1)
            sy = TILE_PX - 1;
        y0 = (int)sy;
        fy = sy - y0;
        y1 = y0 < TILE_PX - 1 ? y0 + 1 : y0;
        for (px = dest_x; px < dest_r; px++) {
            double sx, fx, w00, w10, w01, w11;
            int x0, x1, c, ch[4];
            const uint8_t *p00, *p10, *p01, *p11;
            unsigned char *p;
            if (px < 0 || px >= box->w || !hud_map_content(box, px, py))
                continue;
            sx = (left + ((double)px + 0.5) * 256.0 / (double)span) - (double)ux * 256.0 - 0.5;
            if (sx < 0)
                sx = 0;
            if (sx > TILE_PX - 1)
                sx = TILE_PX - 1;
            x0 = (int)sx;
            fx = sx - x0;
            x1 = x0 < TILE_PX - 1 ? x0 + 1 : x0;
            p00 = rgba + ((size_t)y0 * TILE_PX + (size_t)x0) * 4;
            p10 = rgba + ((size_t)y0 * TILE_PX + (size_t)x1) * 4;
            p01 = rgba + ((size_t)y1 * TILE_PX + (size_t)x0) * 4;
            p11 = rgba + ((size_t)y1 * TILE_PX + (size_t)x1) * 4;
            w11 = fx * fy;
            w10 = fx * (1.0 - fy);
            w01 = (1.0 - fx) * fy;
            w00 = (1.0 - fx) * (1.0 - fy);
            for (c = 0; c < 4; c++)
                ch[c] = (int)(p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11 + 0.5);
            p = pixels + (size_t)py * (size_t)pitch + (size_t)px * (size_t)pix;
            blend_px(p, ch[0], ch[1], ch[2], ch[3], cover);
        }
    }
}

static int tile_ready(const hud_tiles *t, int z, int x, int y, vlc_tick_t now)
{
    int s;
    int n;
    if (z < 2 || z > 18)
        return 0;
    n = 1 << z;
    x = wrap_x(x, n);
    if (y < 0 || y >= n)
        return 0;
    for (s = 0; s < CACHE_N; s++) {
        const tile_slot *slot = &t->slot[s];
        if (slot_match(slot, t->source, z, x, y) && slot_live(slot, now) &&
            !slot->fail && slot->rgba)
            return 1;
    }
    return 0;
}

static int fallback_zoom(const hud_tiles *t, vlc_tick_t now)
{
    int d;
    for (d = 1; d <= 3; d++) {
        int z;
        double mx, my;
        z = t->zoom - d;
        if (z >= 2) {
            hud_map_mercator(t->lat, t->lon, z, &mx, &my);
            if (tile_ready(t, z, (int)floor(mx), (int)floor(my), now))
                return z;
        }
        z = t->zoom + d;
        if (z <= 18) {
            hud_map_mercator(t->lat, t->lon, z, &mx, &my);
            if (tile_ready(t, z, (int)floor(mx), (int)floor(my), now))
                return z;
        }
    }
    return -1;
}

static const uint8_t *sample_world(const hud_tiles *t, int z, double wx, double wy,
                                   vlc_tick_t now)
{
    int n, tx, ty, sx, sy, s;
    int wxi, wyi;
    if (z < 2 || z > 18)
        return NULL;
    n = 1 << z;
    wxi = (int)floor(wx);
    wyi = (int)floor(wy);
    tx = wxi / 256;
    sx = wxi % 256;
    if (sx < 0) {
        sx += 256;
        tx--;
    }
    ty = wyi / 256;
    sy = wyi % 256;
    if (sy < 0) {
        sy += 256;
        ty--;
    }
    if (ty < 0 || ty >= n || sx > 255 || sy > 255)
        return NULL;
    tx = wrap_x(tx, n);
    for (s = 0; s < CACHE_N; s++) {
        const tile_slot *slot = &t->slot[s];
        if (!slot_match(slot, t->source, z, tx, ty))
            continue;
        if (!slot_live(slot, now) || slot->fail || !slot->rgba)
            return NULL;
        return slot->rgba + ((size_t)sy * TILE_PX + (size_t)sx) * 4;
    }
    return NULL;
}

static int paint_scaled(hud_tiles *t, unsigned char *pixels, int pitch, int pix,
                        const hud_map_box *box, int src_z, double left, double top,
                        int span, vlc_tick_t now, int cover)
{
    double step = 256.0 / (double)span;
    double k = ldexp(1.0, src_z - t->zoom);
    int dy, dx, n = 0;
    for (dy = 0; dy < box->h; dy++) {
        for (dx = 0; dx < box->w; dx++) {
            const uint8_t *s;
            unsigned char *p;
            int a;
            if (!hud_map_content(box, dx, dy))
                continue;
            s = sample_world(t, src_z, (left + ((double)dx + 0.5) * step) * k,
                             (top + ((double)dy + 0.5) * step) * k, now);
            if (!s)
                continue;
            a = s[3];
            if (a <= 0)
                continue;
            p = pixels + (size_t)dy * (size_t)pitch + (size_t)dx * (size_t)pix;
            if (cover < 250) {
                int out = a * cover / 255;
                if (out <= 0)
                    continue;
                p[0] = s[0];
                p[1] = s[1];
                p[2] = s[2];
                p[3] = (unsigned char)out;
                n++;
                continue;
            }
            if (a >= 250) {
                p[0] = s[0];
                p[1] = s[1];
                p[2] = s[2];
                p[3] = 255;
            } else {
                int ia = 255 - a;
                p[0] = (unsigned char)((s[0] * a + p[0] * ia) / 255);
                p[1] = (unsigned char)((s[1] * a + p[1] * ia) / 255);
                p[2] = (unsigned char)((s[2] * a + p[2] * ia) / 255);
                p[3] = 255;
            }
            n++;
        }
    }
    return n > 0;
}

void hud_tiles_credit(hud_tiles *t, char *credit, size_t credit_n)
{
    int show;
    if (!credit || credit_n == 0)
        return;
    credit[0] = 0;
    if (!t)
        return;
    vlc_mutex_lock(&t->lock);
    show = t->credit[0] &&
           !((t->source == HUD_TILE_MAPBOX || t->source == HUD_TILE_GOOGLE) && !t->token[0]);
    if (show)
        snprintf(credit, credit_n, "%s", t->credit);
    vlc_mutex_unlock(&t->lock);
}

int hud_tiles_paint(hud_tiles *t, unsigned char *pixels, int pitch, int pix,
                    const hud_map_box *box, char *credit, size_t credit_n,
                    int opacity)
{
    vis_tile vis[VIS_N];
    int nvis = 0;
    double left, top, north, south, east, west;
    double world_w, world_h;
    int span, cx, cy, i, painted = 0;
    int cover;
    vlc_tick_t now;
    if (opacity < 10)
        opacity = 10;
    if (opacity > 100)
        opacity = 100;
    cover = opacity >= 100 ? 255 : opacity * 255 / 100;
    if (credit && credit_n)
        credit[0] = 0;
    if (!t || !pixels || !box)
        return 0;
    vlc_mutex_lock(&t->lock);
    t->scale_gen++;
    if (t->scale_gen == 0)
        t->scale_gen = 1;
    span = t->span >= 64 ? t->span : TILE_PX;
    world_w = (double)box->w * 256.0 / (double)span;
    world_h = (double)box->h * 256.0 / (double)span;
    if (t->view_w >= 8 && t->view_h >= 8 &&
        !((t->source == HUD_TILE_MAPBOX || t->source == HUD_TILE_GOOGLE) && !t->token[0])) {
        view_tiles(t->lat, t->lon, t->zoom, world_w, world_h, vis, &nvis,
                   &left, &top, &north, &south, &east, &west, &cx, &cy);
        now = mdate();
        for (i = 0; i < nvis; i++) {
            int s;
            for (s = 0; s < CACHE_N; s++) {
                tile_slot *slot = &t->slot[s];
                if (!slot_match(slot, t->source, t->zoom, vis[i].x, vis[i].y))
                    continue;
                if (!slot_live(slot, now) || slot->fail || !slot->rgba)
                    break;
                if (span == TILE_PX) {
                    int dest_x = (int)floor((double)vis[i].ux * 256.0 - left);
                    int dest_y = (int)floor((double)vis[i].y * 256.0 - top);
                    blit_copy(pixels, pitch, pix, box, dest_x, dest_y, slot->rgba,
                              TILE_PX, TILE_PX, TILE_PX, cover);
                } else {
                    uint8_t *big = scaled_rgba(t, slot->rgba, span);
                    double scale = (double)span / 256.0;
                    int dest_x = (int)floor(((double)vis[i].ux * 256.0 - left) * scale);
                    int dest_y = (int)floor(((double)vis[i].y * 256.0 - top) * scale);
                    int dest_r = (int)floor((((double)vis[i].ux + 1.0) * 256.0 - left) * scale);
                    int dest_b = (int)floor((((double)vis[i].y + 1.0) * 256.0 - top) * scale);
                    int bw = dest_r - dest_x;
                    int bh = dest_b - dest_y;
                    if (big && bw > 0 && bh > 0) {
                        if (bw > span)
                            bw = span;
                        if (bh > span)
                            bh = span;
                        blit_copy(pixels, pitch, pix, box, dest_x, dest_y, big,
                                  span, bw, bh, cover);
                    } else {
                        blit_span(pixels, pitch, pix, box, vis[i].ux, vis[i].y,
                                  left, top, span, slot->rgba, cover);
                    }
                }
                slot->used = ++t->clock;
                painted++;
                break;
            }
        }
        if (!painted && !box->full) {
            int src_z = fallback_zoom(t, now);
            if (src_z >= 2)
                painted = paint_scaled(t, pixels, pitch, pix, box, src_z,
                                       left, top, span, now, cover);
        }
        if (credit && credit_n)
            snprintf(credit, credit_n, "%s", t->credit);
    }
    vlc_mutex_unlock(&t->lock);
    return painted;
}
