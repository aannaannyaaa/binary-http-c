/* bserve -- serve a directory over BFP/1.
 *
 *     ./bserve ./www 9000 [-v]
 *
 * One thread per connection. Each connection stays open across requests
 * and across errors until the client closes it or sends GOAWAY.
 */
#include "bfp.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define realpath(p, r) _fullpath((r), (p), 4096)
#else
#include <pthread.h>
#include <sys/time.h>
#endif

#define SERVER_NAME  "bserve-c/1.0"
#define IDLE_SECONDS 120

static char root[4096];
static int  verbose;

typedef struct {
    sock_t   s;
    char     peer[64];
    int      last_rejected;   /* stream we already answered with an error */
    frame_t  f;
} conn_t;

static void logmsg(conn_t *c, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[%s] ", c->peer);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void http_date(time_t t, char *out, size_t n)
{
    struct tm *g = gmtime(&t);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", g);
}

static const char *content_type(const char *path)
{
    static const struct { const char *ext, *type; } map[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".css",  "text/css; charset=utf-8" },
        { ".js",   "text/javascript; charset=utf-8" },
        { ".json", "application/json" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".svg",  "image/svg+xml" },
    };
    const char *dot = strrchr(path, '.');
    size_t i;
    if (dot)
        for (i = 0; i < sizeof map / sizeof map[0]; i++)
            if (strcmp(dot, map[i].ext) == 0)
                return map[i].type;
    return "application/octet-stream";
}

/* ---------------------------------------------------------- responses */

static int send_head(conn_t *c, int stream, int status, const char *ctype,
                     long long clen, const char *last_modified, int end)
{
    hblock_t *b = malloc(sizeof *b);
    char num[32], date[64];
    int rc;

    if (!b)
        return -1;
    hb_init(b);
    snprintf(num, sizeof num, "%d", status);
    hb_add(b, ":status", num);
    hb_add(b, "server", SERVER_NAME);
    http_date(time(NULL), date, sizeof date);
    hb_add(b, "date", date);
    hb_add(b, "content-type", ctype);
    snprintf(num, sizeof num, "%lld", clen);
    hb_add(b, "content-length", num);
    if (last_modified)
        hb_add(b, "last-modified", last_modified);   /* literal: not in table */
    rc = frame_send(c->s, T_HEADERS, end ? F_END_STREAM : 0, stream,
                    b->buf, b->n, verbose ? stderr : NULL);
    free(b);
    return rc;
}

static int send_error(conn_t *c, int stream, int status, const char *why)
{
    char body[512];
    int n = snprintf(body, sizeof body, "%d %s\n", status, why);
    if (n < 0 || n >= (int)sizeof body)
        n = (int)strlen(body);
    logmsg(c, "#%d -> %d %s", stream, status, why);
    c->last_rejected = stream;
    if (send_head(c, stream, status, "text/plain; charset=utf-8", n, NULL, 0) < 0)
        return -1;
    return frame_send(c->s, T_DATA, F_END_STREAM, stream, (uint8_t *)body,
                      (size_t)n, verbose ? stderr : NULL);
}

static int send_file(conn_t *c, int stream, const char *file, long long size,
                     const char *lastmod, int head_only)
{
    uint8_t buf[DATA_CHUNK];
    long long left = size;
    FILE *fp;
    int empty = head_only || size == 0;

    if (send_head(c, stream, 200, content_type(file), size, lastmod, empty) < 0)
        return -1;
    if (empty)
        return 0;
    if (!(fp = fopen(file, "rb")))
        return -1;
    while (left > 0) {
        size_t want = left > DATA_CHUNK ? DATA_CHUNK : (size_t)left;
        size_t got = fread(buf, 1, want, fp);
        if (got == 0)
            break;
        left -= (long long)got;
        if (frame_send(c->s, T_DATA, left == 0 ? F_END_STREAM : 0, stream,
                       buf, got, verbose ? stderr : NULL) < 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    if (left > 0)          /* file shrank under us: end the stream anyway */
        return frame_send(c->s, T_DATA, F_END_STREAM, stream, NULL, 0,
                          verbose ? stderr : NULL);
    return 0;
}

/* ---------------------------------------------------------- requests */

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* request path -> file under root. 0 ok, -1 not servable. */
static int resolve(const char *path, char *out, size_t outlen, struct stat *st)
{
    char dec[2048];
    size_t i = 0, j = 0;
    const char *seg;

    /* percent-decode, stop at '?' */
    while (path[i] && path[i] != '?' && j < sizeof dec - 1) {
        if (path[i] == '%' && hexval(path[i + 1]) >= 0 && hexval(path[i + 2]) >= 0) {
            dec[j++] = (char)(hexval(path[i + 1]) * 16 + hexval(path[i + 2]));
            i += 3;
        } else {
            dec[j++] = path[i++];
        }
    }
    dec[j] = '\0';

    /* no NULs, no backslashes, no ".." segment -- nothing escapes root */
    if (strlen(dec) != j || strchr(dec, '\\') || strchr(dec, ':'))
        return -1;
    for (seg = dec; seg; seg = strchr(seg + 1, '/'))
        if (strncmp(seg, "/..", 3) == 0 && (seg[3] == '/' || seg[3] == '\0'))
            return -1;

    if (snprintf(out, outlen, "%s%s", root, dec) >= (int)outlen)
        return -1;
    if (stat(out, st) != 0)
        return -1;
    if (S_ISDIR(st->st_mode)) {
        size_t l = strlen(out);
        if (snprintf(out + l, outlen - l, "%sindex.html",
                     l && out[l - 1] == '/' ? "" : "/") >= (int)(outlen - l))
            return -1;
        if (stat(out, st) != 0)
            return -1;
    }
    return S_ISREG(st->st_mode) ? 0 : -1;
}

static int handle_request(conn_t *c)
{
    frame_t *f = &c->f;
    hfield_t fields[MAX_FIELDS];
    const char *err;
    char method[16], path[2048], file[4096 + 2048], lastmod[64];
    struct stat st;
    int n;

    if (f->stream == 0)
        return send_error(c, 0, 400, "HEADERS on stream 0");
    n = hb_parse(f->payload, f->len, fields, MAX_FIELDS, &err);
    if (n < 0)
        return send_error(c, f->stream, 400, err);
    if (!(f->flags & F_END_STREAM))
        return send_error(c, f->stream, 400, "request HEADERS must set END_STREAM");
    if (hf_count(fields, n, ":method") != 1 || hf_count(fields, n, ":path") != 1)
        return send_error(c, f->stream, 400, "need exactly one :method and one :path");
    if (hf_get(fields, n, ":method", method, sizeof method) < 0 ||
        hf_get(fields, n, ":path", path, sizeof path) < 0)
        return send_error(c, f->stream, 400, "bad :method or :path value");
    if (path[0] != '/')
        return send_error(c, f->stream, 400, ":path must start with /");
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
        return send_error(c, f->stream, 405, "method not allowed");

    if (resolve(path, file, sizeof file, &st) < 0) {
        char why[2100];
        snprintf(why, sizeof why, "not found: %s", path);
        return send_error(c, f->stream, 404, why);
    }
    http_date(st.st_mtime, lastmod, sizeof lastmod);
    logmsg(c, "#%d %s %s -> 200 (%lld bytes)", f->stream, method, path,
           (long long)st.st_size);
    return send_file(c, f->stream, file, (long long)st.st_size, lastmod,
                     strcmp(method, "HEAD") == 0);
}

static void serve(conn_t *c)
{
    logmsg(c, "connected");
    for (;;) {
        int r = frame_read(c->s, &c->f), rc = 0;
        if (r == 0)
            break;
        if (r < 0) {
            logmsg(c, "connection dropped (closed mid-frame or idle)");
            break;
        }
        if (verbose)
            frame_dump(stderr, '<', c->f.raw);

        switch (c->f.type) {
        case T_HEADERS:
            rc = handle_request(c);
            break;
        case T_DATA:
            /* v1 requests carry no body */
            if (c->f.stream != c->last_rejected)
                rc = send_error(c, c->f.stream, 400, "unexpected DATA from client");
            break;
        case T_GOAWAY:
            logmsg(c, "client sent GOAWAY");
            goto done;
        default:
            /* SPEC 3.1: unknown type -- payload already consumed, skip it */
            logmsg(c, "skipped unknown frame type 0x%02x (%u bytes)",
                   c->f.type, c->f.len);
        }
        if (rc < 0)
            break;
    }
done:
    CLOSESOCK(c->s);
    logmsg(c, "closed");
    free(c);
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID arg) { serve(arg); return 0; }
#else
static void *thread_main(void *arg) { serve(arg); return NULL; }
#endif

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    struct stat st;
    sock_t ls;
    int port, i, one = 1, argn = 0;
    const char *args[2];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (argn < 2)
            args[argn++] = argv[i];
    }
    if (argn != 2) {
        fprintf(stderr, "usage: bserve ROOT PORT [-v]\n");
        return 2;
    }
    if (!realpath(args[0], root) || stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "bserve: %s is not a directory\n", args[0]);
        return 2;
    }
    for (i = 0; root[i]; i++)       /* use '/' everywhere, even on Windows */
        if (root[i] == '\\')
            root[i] = '/';
    i = (int)strlen(root);
    if (i > 0 && root[i - 1] == '/')
        root[i - 1] = '\0';
    port = atoi(args[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "bserve: bad port %s\n", args[1]);
        return 2;
    }

    if (net_init() < 0)
        return 1;
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(ls, 16) != 0) {
        fprintf(stderr, "bserve: cannot listen on port %d\n", port);
        return 1;
    }
    fprintf(stderr, "bserve: serving %s on port %d\n", root, port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        sock_t s = accept(ls, (struct sockaddr *)&peer, &plen);
        conn_t *c;
        if (s == INVALID_SOCKET)
            continue;
        c = calloc(1, sizeof *c);
        if (!c) {
            CLOSESOCK(s);
            continue;
        }
        c->s = s;
        c->last_rejected = -1;
        snprintf(c->peer, sizeof c->peer, "%s:%d", inet_ntoa(peer.sin_addr),
                 ntohs(peer.sin_port));
#ifdef _WIN32
        {
            DWORD ms = IDLE_SECONDS * 1000;
            HANDLE h;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
            h = CreateThread(NULL, 0, thread_main, c, 0, NULL);
            if (h) CloseHandle(h); else serve(c);
        }
#else
        {
            struct timeval tv = { IDLE_SECONDS, 0 };
            pthread_t t;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            if (pthread_create(&t, NULL, thread_main, c) == 0)
                pthread_detach(t);
            else
                serve(c);
        }
#endif
    }
}
