# Annotated hexdump — a full BFP/1 request and response

Captured live:

```
./bserve ./www 9100
./bcurl -v localhost:9100/index.html
```

`>` means bcurl sent it, `<` means bcurl received it. Offsets are from the
start of each frame. Every byte is real and every byte is explained.

---

## 1 · `>` HEADERS — the request · 6 + 57 = 63 bytes

```
0000  00 39 01 01 00 01 81 00 03 47 45 54 82 00 0b 2f  |.9.......GET.../|
0010  69 6e 64 65 78 2e 68 74 6d 6c 84 00 0e 6c 6f 63  |index.html...loc|
0020  61 6c 68 6f 73 74 3a 39 31 30 30 85 00 0b 62 63  |alhost:9100...bc|
0030  75 72 6c 2d 63 2f 31 2e 30 86 00 03 2a 2f 2a     |url-c/1.0...*/*|
```

| Offset | Bytes | Field |
|---|---|---|
| 00–01 | `00 39` | **Length** 0x0039 = 57 |
| 02 | `01` | **Type** HEADERS |
| 03 | `01` | **Flags** END_STREAM — request is complete, no body |
| 04–05 | `00 01` | **Stream** 1 |
| 06 | `81` | top bit set → static #1 `:method` |
| 07–08 | `00 03` | value length 3 |
| 09–0b | `47 45 54` | `GET` |
| 0c | `82` | static #2 `:path` |
| 0d–0e | `00 0b` | length 11 |
| 0f–19 | `2f 69 … 6c` | `/index.html` |
| 1a | `84` | static #4 `:authority` |
| 1b–1c | `00 0e` | length 14 |
| 1d–2a | `6c 6f … 30` | `localhost:9100` |
| 2b | `85` | static #5 `user-agent` |
| 2c–2d | `00 0b` | length 11 |
| 2e–38 | `62 63 … 30` | `bcurl-c/1.0` |
| 39 | `86` | static #6 `accept` |
| 3a–3b | `00 03` | length 3 |
| 3c–3e | `2a 2f 2a` | `*/*` |

Check: 6 + 14 + 17 + 14 + 6 = **57**. ✓

---

## 2 · `<` HEADERS — response head · 6 + 130 = 136 bytes

```
0000  00 82 01 00 00 01 83 00 03 32 30 30 89 00 0c 62  |.........200...b|
0010  73 65 72 76 65 2d 63 2f 31 2e 30 8a 00 1d 57 65  |serve-c/1.0...We|
0020  64 2c 20 30 37 20 4f 63 74 20 32 30 32 36 20 31  |d, 07 Oct 2026 1|
0030  36 3a 31 34 3a 30 38 20 47 4d 54 87 00 18 74 65  |6:14:08 GMT...te|
0040  78 74 2f 68 74 6d 6c 3b 20 63 68 61 72 73 65 74  |xt/html; charset|
0050  3d 75 74 66 2d 38 88 00 02 36 31 0d 6c 61 73 74  |=utf-8...61.last|
0060  2d 6d 6f 64 69 66 69 65 64 00 1d 57 65 64 2c 20  |-modified..Wed, |
0070  30 37 20 4f 63 74 20 32 30 32 36 20 31 36 3a 31  |07 Oct 2026 16:1|
0080  34 3a 30 34 20 47 4d 54                          |4:04 GMT|
```

| Offset | Bytes | Field |
|---|---|---|
| 00–01 | `00 82` | **Length** 130 |
| 02 | `01` | **Type** HEADERS |
| 03 | `00` | **Flags** none — DATA follows |
| 04–05 | `00 01` | **Stream** 1 — the answer to request 1 |
| 06 | `83` | static #3 `:status` |
| 07–08 | `00 03` | length 3 |
| 09–0b | `32 30 30` | `200` |
| 0c | `89` | static #9 `server` |
| 0d–0e | `00 0c` | length 12 |
| 0f–1a | `62 73 … 30` | `bserve-c/1.0` |
| 1b | `8a` | static #10 `date` |
| 1c–1d | `00 1d` | length 29 |
| 1e–3a | `57 65 … 54` | `Wed, 07 Oct 2026 16:14:08 GMT` |
| 3b | `87` | static #7 `content-type` |
| 3c–3d | `00 18` | length 24 |
| 3e–55 | `74 65 … 38` | `text/html; charset=utf-8` |
| 56 | `88` | static #8 `content-length` |
| 57–58 | `00 02` | length 2 |
| 59–5a | `36 31` | `61` |
| 5b | `0d` | **top bit clear → literal name**, 13 bytes long |
| 5c–68 | `6c 61 … 64` | `last-modified` |
| 69–6a | `00 1d` | length 29 |
| 6b–87 | `57 65 … 54` | `Wed, 07 Oct 2026 16:14:04 GMT` |

Check: 6 + 15 + 32 + 27 + 5 + 45 = **130**. ✓
Five indexed names cost 1 byte each; the one literal name costs 14.

---

## 3 · `<` DATA — the body · 6 + 61 = 67 bytes

```
0000  00 3d 00 01 00 01 3c 21 64 6f 63 74 79 70 65 20  |.=....<!doctype |
0010  68 74 6d 6c 3e 0a 3c 74 69 74 6c 65 3e 42 46 50  |html>.<title>BFP|
0020  3c 2f 74 69 74 6c 65 3e 0a 3c 68 31 3e 48 65 6c  |</title>.<h1>Hel|
0030  6c 6f 20 6f 76 65 72 20 42 46 50 2f 31 3c 2f 68  |lo over BFP/1</h|
0040  31 3e 0a                                         |1>.|
```

| Offset | Bytes | Field |
|---|---|---|
| 00–01 | `00 3d` | **Length** 61 = `content-length` ✓ |
| 02 | `00` | **Type** DATA |
| 03 | `01` | **Flags** END_STREAM — response 1 is done |
| 04–05 | `00 01` | **Stream** 1 |
| 06–42 | `3c 21 … 0a` | the 61 bytes of `www/index.html`, copied to stdout |

---

## 4 · `>` GOAWAY — client signs off · 6 + 3 = 9 bytes

```
0000  00 03 07 00 00 00 62 79 65                       |......bye|
```

| Offset | Bytes | Field |
|---|---|---|
| 00–01 | `00 03` | **Length** 3 |
| 02 | `07` | **Type** GOAWAY |
| 03 | `00` | **Flags** none |
| 04–05 | `00 00` | **Stream** 0 — about the connection |
| 06–08 | `62 79 65` | reason `bye`, then the socket closes |

**Totals:** request 63 bytes, response 136 + 67 = 203 bytes, of which 12 are
frame headers.

---

## Bonus · an unknown frame being skipped

`./bcurl --probe ...` first sends a type the server has never heard of:

```
0000  00 0d fa 00 00 00 68 65 6c 6c 6f 20 66 72 6f 6d  |......hello from|
0010  20 76 32                                         | v2|
```

Length `00 0d` = 13, Type `fa` (unknown), stream 0, payload `hello from v2`.
The server reads 13 bytes, drops them, logs
`skipped unknown frame type 0xfa (13 bytes)`, and serves the next request.
