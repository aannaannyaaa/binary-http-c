# BFP/1 — HTTP over binary frames, in C

A server (`bserve`) and a client (`bcurl`) that speak **BFP/1**, a binary
framing of HTTP GET/HEAD over a single TCP connection. The spec is the
contract; the code is one implementation of it.

| | |
|---|---|
| [SPEC.md](SPEC.md) | **The spec** — two pages, written for a stranger |
| [HEXDUMP.md](HEXDUMP.md) | **Annotated hexdump** of one full request + response, every byte |
| [src/bserve.c](src/bserve.c) | Track 1 — the server |
| [src/bcurl.c](src/bcurl.c) | Track 2 — the client |
| [src/bfp.c](src/bfp.c), [src/bfp.h](src/bfp.h) | Frame I/O, header codec, hexdump |
| [tests/test_bfp.py](tests/test_bfp.py) | 21 end-to-end tests using hand-packed bytes |
| [www/](www) | Sample files to serve |

## Build

Plain C99, no dependencies. Linux / macOS:

```bash
make
```

Windows (MinGW-w64 gcc):

```bash
mingw32-make
```

or without make:

```bash
gcc -O2 -o bserve.exe src/bserve.c src/bfp.c -lws2_32
gcc -O2 -o bcurl.exe src/bcurl.c src/bfp.c -lws2_32
```

## Run

```bash
./bserve ./www 9000
```

```bash
./bcurl -v localhost:9000/index.html
```

### bserve — `./bserve ROOT PORT [-v]`

- accepts TCP connections, one thread each
- reads binary request frames, maps `:path` to a file under ROOT
  (directory → `index.html`; `..`, `\`, `:` → refused)
- replies with `:status`, headers, then the file in ≤16 KiB DATA frames
- **404** if the file is not there, **400** if the frame is malformed,
  **405** for anything but GET/HEAD
- **keeps the connection open** after every response, errors included
- skips frame types it does not know
- `-v` hexdumps every frame (`<` received, `>` sent)

### bcurl — `./bcurl [-v] [-I] [--probe] host:port/path [/more/paths ...]`

- builds the binary request frame and writes the body to **stdout**
- `-v` hexdumps every frame to stderr (`>` sent, `<` received)
- extra paths are fetched **on the same connection** — a different
  host:port is refused, bcurl **never opens a second connection**
- `-I` sends HEAD; `--probe` first sends an unknown frame type (0xFA)
  to check the server skips it
- exit codes follow curl: `0` ok, **`22` on any 4xx/5xx**, `7` cannot
  connect, `8` bad reply, `2` usage

```bash
./bcurl localhost:9000/index.html /docs/notes.txt /missing; echo $?   # prints 22
```

## Tests

```bash
make test
```

(or `python tests/test_bfp.py` after building). The tests never use the C
codec: they build frames byte-by-byte from SPEC.md, so they check the
**protocol**, not just that this client agrees with this server. Covered:
200, HEAD, directory index, 404, three traversal tricks, 405, every 400
case, stream 0, unknown frame types, 7 requests on one connection, a 51 KB
file split into DATA frames, bcurl's stdout and exit codes, and bcurl
against a hand-written fake server that sends an unknown frame and a
split body.

## The design, in short

6-byte header **Length 16 · Type 8 · Flags 8 · Stream 16** — HTTP/2's
24/8/8/31 trimmed for one-request-at-a-time file serving (reasons in
SPEC §2). Header names: top bit set = one byte from a 10-entry static
table, top bit clear = 7-bit length + literal name (HPACK's indexed/literal
split). And the rule that keeps the door open for v2: **an unknown frame
type is skipped, never an error.**
