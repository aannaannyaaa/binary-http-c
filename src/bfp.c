/* bfp.c -- framing, header codec and hexdump for BFP/1. */
#include "bfp.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#endif

const char *STATIC_TABLE[STATIC_COUNT] = {
    ":method",          /* 0x81 */
    ":path",            /* 0x82 */
    ":status",          /* 0x83 */
    ":authority",       /* 0x84 */
    "user-agent",       /* 0x85 */
    "accept",           /* 0x86 */
    "content-type",     /* 0x87 */
    "content-length",   /* 0x88 */
    "server",           /* 0x89 */
    "date",             /* 0x8a */
};

/* ------------------------------------------------------------ sockets */

int net_init(void)
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
#else
    signal(SIGPIPE, SIG_IGN);
    return 0;
#endif
}

int send_all(sock_t s, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        int k = send(s, p, (int)(n > INT_MAX ? INT_MAX : n), 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

int recv_all(sock_t s, void *buf, size_t n)
{
    char *p = buf;
    size_t got = 0;
    while (got < n) {
        int k = recv(s, p + got, (int)(n - got), 0);
        if (k == 0 && got == 0)
            return 0;
        if (k <= 0)
            return -1;
        got += (size_t)k;
    }
    return 1;
}

/* ------------------------------------------------------------- frames */

int frame_read(sock_t s, frame_t *f)
{
    int r = recv_all(s, f->raw, HDR_LEN);
    if (r <= 0)
        return r;
    f->len    = (uint16_t)(f->raw[0] << 8 | f->raw[1]);
    f->type   = f->raw[2];
    f->flags  = f->raw[3];
    f->stream = (uint16_t)(f->raw[4] << 8 | f->raw[5]);
    f->payload = f->raw + HDR_LEN;
    if (f->len > 0 && recv_all(s, f->payload, f->len) != 1)
        return -1;                  /* closed mid-frame: unrecoverable */
    return 1;
}

int frame_send(sock_t s, int type, int flags, int stream,
               const uint8_t *payload, size_t n, FILE *dump)
{
    uint8_t *buf;
    int rc;

    if (n > MAX_PAYLOAD)
        return -1;
    buf = malloc(HDR_LEN + n);
    if (!buf)
        return -1;
    buf[0] = (uint8_t)(n >> 8);
    buf[1] = (uint8_t)n;
    buf[2] = (uint8_t)type;
    buf[3] = (uint8_t)flags;
    buf[4] = (uint8_t)(stream >> 8);
    buf[5] = (uint8_t)stream;
    if (n)
        memcpy(buf + HDR_LEN, payload, n);
    if (dump)
        frame_dump(dump, '>', buf);
    rc = send_all(s, buf, HDR_LEN + n);
    free(buf);
    return rc;
}

static const char *type_name(int t)
{
    switch (t) {
    case T_DATA:    return "DATA";
    case T_HEADERS: return "HEADERS";
    case T_GOAWAY:  return "GOAWAY";
    default:        return NULL;
    }
}

void frame_dump(FILE *o, char dir, const uint8_t *raw)
{
    size_t len = (size_t)(raw[0] << 8 | raw[1]);
    int type = raw[2], flags = raw[3], stream = raw[4] << 8 | raw[5];
    const char *tn = type_name(type);
    size_t total = HDR_LEN + len, shown, off;

    if (tn)
        fprintf(o, "%c %s len=%zu flags=%s stream=%d\n", dir, tn, len,
                flags & F_END_STREAM ? "END_STREAM" : "-", stream);
    else
        fprintf(o, "%c UNKNOWN(0x%02x) len=%zu flags=0x%02x stream=%d\n",
                dir, type, len, flags, stream);

    if (type == T_HEADERS) {
        hfield_t f[MAX_FIELDS];
        const char *err;
        int i, n = hb_parse(raw + HDR_LEN, len, f, MAX_FIELDS, &err);
        if (n < 0)
            fprintf(o, "    (malformed: %s)\n", err);
        for (i = 0; i < n; i++)
            fprintf(o, "    %s: %.*s\n", f[i].name, (int)f[i].vlen, (const char *)f[i].val);
    }

    /* every frame is dumped; big DATA frames are cut at 256 payload bytes */
    shown = total > HDR_LEN + 256 ? HDR_LEN + 256 : total;
    for (off = 0; off < shown; off += 16) {
        size_t i;
        fprintf(o, "  %04zx  ", off);
        for (i = 0; i < 16; i++) {
            if (off + i < shown)
                fprintf(o, "%02x ", raw[off + i]);
            else
                fputs("   ", o);
        }
        fputs(" |", o);
        for (i = 0; i < 16 && off + i < shown; i++) {
            uint8_t c = raw[off + i];
            fputc(c >= 32 && c < 127 ? c : '.', o);
        }
        fputs("|\n", o);
    }
    if (shown < total)
        fprintf(o, "  ... %zu more bytes\n", total - shown);
    fflush(o);
}

/* ------------------------------------------------------- header block */

static int static_index(const char *name)
{
    int i;
    for (i = 0; i < STATIC_COUNT; i++)
        if (strcmp(STATIC_TABLE[i], name) == 0)
            return i + 1;
    return 0;
}

static int valid_name_char(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == ':';
}

void hb_init(hblock_t *b) { b->n = 0; }

int hb_add(hblock_t *b, const char *name, const char *value)
{
    size_t nl = strlen(name), vl = strlen(value);
    int idx = static_index(name);
    size_t need = (idx ? 1 : 1 + nl) + 2 + vl;

    if (vl > 0xFFFF || b->n + need > MAX_PAYLOAD)
        return -1;
    if (idx) {
        b->buf[b->n++] = (uint8_t)(0x80 | idx);
    } else {
        if (nl < 1 || nl > 127)
            return -1;
        b->buf[b->n++] = (uint8_t)nl;
        memcpy(b->buf + b->n, name, nl);
        b->n += nl;
    }
    b->buf[b->n++] = (uint8_t)(vl >> 8);
    b->buf[b->n++] = (uint8_t)vl;
    memcpy(b->buf + b->n, value, vl);
    b->n += vl;
    return 0;
}

int hb_parse(const uint8_t *p, size_t n, hfield_t *out, int max, const char **err)
{
    size_t i = 0;
    int count = 0;

    while (i < n) {
        uint8_t b = p[i++];
        hfield_t *f;
        size_t vlen;

        if (count == max) { *err = "too many header fields"; return -1; }
        f = &out[count];

        if (b & 0x80) {
            int idx = b & 0x7F;
            if (idx < 1 || idx > STATIC_COUNT) { *err = "unknown static index"; return -1; }
            strcpy(f->name, STATIC_TABLE[idx - 1]);
        } else {
            size_t k;
            if (b == 0) { *err = "zero-length literal name"; return -1; }
            if (i + b > n) { *err = "literal name runs past end of frame"; return -1; }
            for (k = 0; k < b; k++) {
                if (!valid_name_char(p[i + k])) { *err = "illegal character in name"; return -1; }
                f->name[k] = (char)p[i + k];
            }
            f->name[b] = '\0';
            i += b;
        }
        if (i + 2 > n) { *err = "truncated value length"; return -1; }
        vlen = (size_t)(p[i] << 8 | p[i + 1]);
        i += 2;
        if (i + vlen > n) { *err = "value runs past end of frame"; return -1; }
        f->val = p + i;
        f->vlen = vlen;
        i += vlen;
        count++;
    }
    return count;
}

int hf_count(const hfield_t *f, int n, const char *name)
{
    int i, c = 0;
    for (i = 0; i < n; i++)
        if (strcmp(f[i].name, name) == 0)
            c++;
    return c;
}

/* copy the first value of `name` as a C string; -1 if absent, has NUL, or too long */
int hf_get(const hfield_t *f, int n, const char *name, char *dst, size_t dstlen)
{
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(f[i].name, name) != 0)
            continue;
        if (f[i].vlen >= dstlen || memchr(f[i].val, 0, f[i].vlen))
            return -1;
        memcpy(dst, f[i].val, f[i].vlen);
        dst[f[i].vlen] = '\0';
        return 0;
    }
    return -1;
}
