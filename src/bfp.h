/* bfp.h -- BFP/1 wire format, shared by bserve and bcurl.
 *
 * Frame header, 6 bytes:  length:16 | type:8 | flags:8 | stream:16
 * See SPEC.md for the full protocol.
 */
#ifndef BFP_H
#define BFP_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

#define HDR_LEN      6
#define MAX_PAYLOAD  65535      /* the most a 16-bit length can say */
#define DATA_CHUNK   16384      /* what we put in one DATA frame    */

/* frame types -- same numbers HTTP/2 uses for the same jobs */
#define T_DATA     0x0
#define T_HEADERS  0x1
#define T_GOAWAY   0x7

#define F_END_STREAM 0x1

typedef struct {
    uint16_t len;
    uint8_t  type, flags;
    uint16_t stream;
    uint8_t *payload;                     /* points into raw */
    uint8_t  raw[HDR_LEN + MAX_PAYLOAD];  /* header + payload as received */
} frame_t;

/* header block: 0x80|i = static name i (1..10), 0x01..0x7F = literal
 * name of that many bytes follows; then a 16-bit value length + value. */
#define STATIC_COUNT 10
extern const char *STATIC_TABLE[STATIC_COUNT];

typedef struct {
    uint8_t buf[MAX_PAYLOAD];
    size_t  n;
} hblock_t;

typedef struct {
    char           name[128];
    const uint8_t *val;
    size_t         vlen;
} hfield_t;

#define MAX_FIELDS 64

int  net_init(void);
int  send_all(sock_t s, const void *buf, size_t n);
int  recv_all(sock_t s, void *buf, size_t n);         /* 1 ok, 0 EOF, -1 error */

int  frame_read(sock_t s, frame_t *f);                /* 1 ok, 0 EOF, -1 error */
int  frame_send(sock_t s, int type, int flags, int stream,
                const uint8_t *payload, size_t n, FILE *dump);
void frame_dump(FILE *o, char dir, const uint8_t *raw);

void hb_init(hblock_t *b);
int  hb_add(hblock_t *b, const char *name, const char *value);
int  hb_parse(const uint8_t *p, size_t n, hfield_t *out, int max, const char **err);
int  hf_count(const hfield_t *f, int n, const char *name);
int  hf_get(const hfield_t *f, int n, const char *name, char *dst, size_t dstlen);

#endif
