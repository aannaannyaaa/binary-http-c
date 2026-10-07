# BFP/1 — Binary Frame Protocol for HTTP

BFP/1 moves HTTP-style GET/HEAD requests and responses as binary frames over
**one** TCP connection. MUST / SHOULD / MAY follow RFC 2119; every integer is
unsigned big-endian.

## 1. Connection model

- The client opens one connection and sends every request on it. It MUST NOT
  open a second one.
- The server answers in arrival order and keeps the connection open — after
  errors too.
- No handshake: byte 0 is a frame header. Either side may end with GOAWAY (§3)
  and close.

## 2. Frame layout

```
 0               1               2               3
+-------------------------------+---------------+---------------+
|          Length (16)          |   Type (8)    |   Flags (8)   |
+-------------------------------+---------------+---------------+
|        Stream ID (16)         |  Payload (Length bytes) ...   |
+-------------------------------+-------------------------------+
```

| Field | Bits | Meaning |
|---|---|---|
| Length | 16 | Payload bytes after the 6-byte header (0–65 535) |
| Type | 8 | Payload kind (§3) |
| Flags | 8 | 0x01 = END_STREAM; other bits sent 0, ignored on receipt |
| Stream ID | 16 | Request this frame belongs to; 0 = the connection |

### Why 16 / 8 / 8 / 16 instead of HTTP/2's 24 / 8 / 8 / 31

HTTP/2 is sized for a browser multiplexing hundreds of streams for hours. We
serve one file at a time, so we trimmed it:

- **Length 16.** HTTP/2 itself sends at most 16 KiB per frame unless SETTINGS
  raise it, and we have no SETTINGS. A 16-bit length means a receiver can hold
  any frame in one fixed 64 KiB buffer: no "frame too large" case exists at
  all. Bigger files are just more DATA frames.
- **Type 8, Flags 8.** Unchanged from HTTP/2, type numbers included (DATA 0x0,
  HEADERS 0x1, GOAWAY 0x7), so an HTTP/2 reader feels at home. 253 free types
  are the room for v2.
- **Stream 16.** An ID only has to be unique among requests in flight on one
  connection — in v1 that is one. Clients count from 1 and wrap after 65 535.
- **6 bytes total** instead of 9, with both 16-bit fields 2-byte aligned.

## 3. Frame types

| Type | Name | Stream | Payload |
|---|---|---|---|
| 0x0 | DATA | ≠ 0 | Body bytes; senders SHOULD keep each ≤ 16 384 |
| 0x1 | HEADERS | ≠ 0 | Header block (§4); opens a request or response |
| 0x7 | GOAWAY | 0 | Optional UTF-8 reason; sender then closes |

END_STREAM marks the final frame of a request or response.

### 3.1 Unknown types — MUST skip

**On a Type it does not know, a receiver MUST read Length bytes, drop them,
and continue as if the frame was never sent** — no reply, no error, no close.
Length sits in the same place in every frame, so any v1 peer can step over
any future frame. That is how v2 adds frames without breaking v1.

## 4. Header block

Fields run back to back until the payload ends. The **top bit of the first
byte** picks the name encoding — HPACK's indexed / literal split:

```
indexed:  |1| index (7) | value-len (16) | value |
literal:  |0| n-len (7) | name (n-len)   | value-len (16) | value |
```

- **Indexed:** low 7 bits = 1–10 from the table below. 11–127 are reserved
  and malformed in v1.
- **Literal:** low 7 bits = name length 1–127 (0 is malformed), then the name
  in lowercase `a-z 0-9 - _ . :`.
- **Value:** 16-bit length + UTF-8 bytes, no NUL. Numbers are ASCII (`200`).

| Byte | Name | Byte | Name |
|---|---|---|---|
| 0x81 | `:method` | 0x86 | `accept` |
| 0x82 | `:path` | 0x87 | `content-type` |
| 0x83 | `:status` | 0x88 | `content-length` |
| 0x84 | `:authority` | 0x89 | `server` |
| 0x85 | `user-agent` | 0x8a | `date` |

These are exactly the ten names our programs send; anything else (e.g.
`last-modified`) goes literal.

## 5. Messages

**Request** — one HEADERS frame with END_STREAM, stream ≠ 0, exactly one
`:method` (`GET`/`HEAD`) and one `:path` beginning with `/`. `:authority`,
`user-agent`, `accept` SHOULD be sent. No body in v1.

**Response** — HEADERS on the same stream (`:status` MUST; `content-type`,
`content-length`, `server`, `date` SHOULD), then DATA frames. The last frame
carries END_STREAM; with no body (HEAD, empty file) that is the HEADERS frame.

**Files** — `:path` is percent-decoded, `?query` dropped, and resolved under
the server root; a directory serves its `index.html`. Paths with a `..`
segment, `\` or `:` MUST NOT be served.

| Status | When |
|---|---|
| 200 | Found; body is the file |
| 400 | Malformed: reserved / truncated header block, missing or repeated `:method`/`:path`, `:path` without `/`, request lacking END_STREAM, HEADERS on stream 0, DATA from a client |
| 404 | Not found or outside the root |
| 405 | Method other than GET/HEAD |

Errors carry a one-line `text/plain` body. **A 400 goes on the stream the bad
frame named (0 if it named 0) and the connection stays open** — Length always
marks the next frame. Only a drop mid-frame is beyond repair.

## 6. Room for version 2

Unknown types are skipped (§3.1), unknown flags ignored (§2), indexes 11–127
are free for a bigger table, and literal names leave the top-bit-clear space
open. Every byte of a real exchange is in [HEXDUMP.md](HEXDUMP.md).
