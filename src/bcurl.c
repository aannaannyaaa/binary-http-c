/* bcurl -- fetch over BFP/1.
 *
 *     ./bcurl [-v] [-I] [--probe] localhost:9000/index.html [/more /paths ...]
 *
 * All paths go over ONE connection, in order. Body to stdout, -v hexdumps
 * every frame to stderr.
 *
 * Exit status (curl's numbers): 0 ok, 22 a 4xx/5xx, 7 could not connect,
 * 8 bad reply from server, 2 usage.
 */
#include "bfp.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define USER_AGENT   "bcurl-c/1.0"
#define PROBE_TYPE   0xFA

static int verbose;

/* "[scheme://]host[:port][/path]" */
static int parse_url(const char *url, char *host, size_t hl, int *port, char *path, size_t pl)
{
    const char *p = strstr(url, "://"), *slash, *colon;
    size_t n;

    if (p)
        url = p + 3;
    slash = strchr(url, '/');
    if (!slash)
        slash = url + strlen(url);
    colon = memchr(url, ':', (size_t)(slash - url));
    n = (size_t)((colon ? colon : slash) - url);
    if (n == 0 || n >= hl)
        return -1;
    memcpy(host, url, n);
    host[n] = '\0';
    *port = colon ? atoi(colon + 1) : 9000;
    if (*port <= 0 || *port > 65535)
        return -1;
    snprintf(path, pl, "%s", *slash ? slash : "/");
    return 0;
}

static sock_t dial(const char *host, int port)
{
    struct addrinfo hints, *res, *ai;
    char ps[8];
    sock_t s = INVALID_SOCKET;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0)
        return INVALID_SOCKET;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        CLOSESOCK(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

/* one request/response. returns the status, or -1 on a protocol error */
static int fetch(sock_t s, int stream, const char *method, const char *path,
                 const char *authority, frame_t *f)
{
    hblock_t *b = malloc(sizeof *b);
    int status = -1;
    long long got = 0, expect = -1;
    FILE *dump = verbose ? stderr : NULL;

    if (!b)
        return -1;
    hb_init(b);
    hb_add(b, ":method", method);
    hb_add(b, ":path", path);
    hb_add(b, ":authority", authority);
    hb_add(b, "user-agent", USER_AGENT);
    hb_add(b, "accept", "*/*");
    if (frame_send(s, T_HEADERS, F_END_STREAM, stream, b->buf, b->n, dump) < 0) {
        free(b);
        fprintf(stderr, "bcurl: send failed\n");
        return -1;
    }
    free(b);

    for (;;) {
        int r = frame_read(s, f);
        if (r <= 0) {
            fprintf(stderr, "bcurl: server closed the connection mid-response\n");
            return -1;
        }
        if (dump)
            frame_dump(dump, '<', f->raw);

        if (f->type == T_GOAWAY) {
            fprintf(stderr, "bcurl: server sent GOAWAY: %.*s\n", f->len, (char *)f->payload);
            return -1;
        }
        if (f->type != T_HEADERS && f->type != T_DATA)
            continue;                         /* SPEC 3.1: skip unknown */
        if (f->stream != stream) {
            fprintf(stderr, "bcurl: ignoring frame for stream %d\n", f->stream);
            continue;
        }

        if (f->type == T_HEADERS) {
            hfield_t fields[MAX_FIELDS];
            const char *err;
            char v[32];
            int n = hb_parse(f->payload, f->len, fields, MAX_FIELDS, &err);
            if (n < 0) {
                fprintf(stderr, "bcurl: malformed response headers: %s\n", err);
                return -1;
            }
            if (status != -1 || hf_get(fields, n, ":status", v, sizeof v) < 0) {
                fprintf(stderr, "bcurl: response without a single :status\n");
                return -1;
            }
            status = atoi(v);
            if (hf_get(fields, n, "content-length", v, sizeof v) == 0)
                expect = atoll(v);
        } else {
            if (status == -1) {
                fprintf(stderr, "bcurl: DATA before HEADERS\n");
                return -1;
            }
            fwrite(f->payload, 1, f->len, stdout);
            got += f->len;
        }
        if (f->flags & F_END_STREAM)
            break;
    }
    fflush(stdout);
    if (strcmp(method, "HEAD") != 0 && expect >= 0 && expect != got)
        fprintf(stderr, "bcurl: warning: content-length %lld but got %lld bytes\n", expect, got);
    if (verbose)
        fprintf(stderr, "* %s %s -> %d (%lld bytes)\n", method, path, status, got);
    return status;
}

int main(int argc, char **argv)
{
    const char *method = "GET";
    const char *urls[64];
    int nurls = 0, probe = 0, i, port, rc = 0, stream = 1;
    char host[256], path[2048], authority[300];
    frame_t *f;
    sock_t s;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-I") == 0) method = "HEAD";
        else if (strcmp(argv[i], "--probe") == 0) probe = 1;
        else if (nurls < 64) urls[nurls++] = argv[i];
    }
    if (nurls == 0 || parse_url(urls[0], host, sizeof host, &port, path, sizeof path) < 0) {
        fprintf(stderr, "usage: bcurl [-v] [-I] [--probe] host:port/path [/more ...]\n");
        return 2;
    }
    /* every extra URL must be a bare path or the same host:port */
    for (i = 1; i < nurls; i++) {
        char h2[256], p2[2048];
        int port2;
        if (urls[i][0] == '/')
            continue;
        if (parse_url(urls[i], h2, sizeof h2, &port2, p2, sizeof p2) < 0 ||
            strcmp(h2, host) != 0 || port2 != port) {
            fprintf(stderr, "bcurl: %s needs a second connection; bcurl never opens one\n", urls[i]);
            return 2;
        }
    }

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (net_init() < 0)
        return 7;
    s = dial(host, port);
    if (s == INVALID_SOCKET) {
        fprintf(stderr, "bcurl: cannot connect to %s:%d\n", host, port);
        return 7;
    }
    snprintf(authority, sizeof authority, "%s:%d", host, port);
    f = malloc(sizeof *f);
    if (!f)
        return 8;

    if (probe) {
        static const char msg[] = "hello from v2";
        frame_send(s, PROBE_TYPE, 0, 0, (const uint8_t *)msg, sizeof msg - 1,
                   verbose ? stderr : NULL);
    }

    for (i = 0; i < nurls; i++) {
        char p[2048];
        int status, port2;
        char h2[256];
        if (urls[i][0] == '/')
            snprintf(p, sizeof p, "%s", urls[i]);
        else
            parse_url(urls[i], h2, sizeof h2, &port2, p, sizeof p);
        status = fetch(s, stream, method, p, authority, f);
        stream = stream == 0xFFFF ? 1 : stream + 1;
        if (status < 0) {
            rc = 8;
            break;
        }
        if (status >= 400) {
            fprintf(stderr, "bcurl: %s -> %d\n", p, status);
            rc = 22;
        }
    }

    frame_send(s, T_GOAWAY, 0, 0, (const uint8_t *)"bye", 3, verbose ? stderr : NULL);
    CLOSESOCK(s);
    free(f);
    return rc;
}
