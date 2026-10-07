"""BFP/1 wire format -- the Python twin of src/bfp.c.

Frame header (6 bytes, big-endian):  length:16  type:8  flags:8  stream:16
Header field:  0x80|i (static name i) or n (literal name of n bytes) + name,
               then value-length:16 + value.
"""

import struct
from dataclasses import dataclass

HEADER = struct.Struct(">HBBH")
HEADER_LEN = HEADER.size            # 6
DATA_CHUNK = 16384

DATA, HEADERS, GOAWAY = 0x0, 0x1, 0x7
KIND = {DATA: "DATA", HEADERS: "HEADERS", GOAWAY: "GOAWAY"}
END_STREAM = 0x01

STATIC = (":method", ":path", ":status", ":authority", "user-agent",
          "accept", "content-type", "content-length", "server", "date")
INDEX = {name: i for i, name in enumerate(STATIC, start=1)}
NAME_CHARS = frozenset(b"abcdefghijklmnopqrstuvwxyz0123456789-_.:")


class Malformed(ValueError):
    """Frame boundaries are fine, contents are not -> 400."""


@dataclass
class Frame:
    type: int
    flags: int
    stream: int
    payload: bytes = b""

    @property
    def end(self):
        return bool(self.flags & END_STREAM)

    def encode(self):
        return HEADER.pack(len(self.payload), self.type, self.flags, self.stream) + self.payload

    @classmethod
    def headers(cls, stream, fields, end=False):
        return cls(HEADERS, END_STREAM if end else 0, stream, encode_fields(fields))


def encode_fields(fields):
    out = bytearray()
    for name, value in fields:
        raw = value.encode() if isinstance(value, str) else value
        if name in INDEX:
            out.append(0x80 | INDEX[name])
        else:
            n = name.encode("ascii")
            assert 1 <= len(n) <= 127, name
            out.append(len(n))
            out += n
        out += struct.pack(">H", len(raw)) + raw
    return bytes(out)


def decode_fields(buf):
    fields, pos = [], 0
    while pos < len(buf):
        lead = buf[pos]
        pos += 1
        if lead & 0x80:
            idx = lead & 0x7F
            if not 1 <= idx <= len(STATIC):
                raise Malformed("unknown static index %d" % idx)
            name = STATIC[idx - 1]
        else:
            if lead == 0:
                raise Malformed("zero-length literal name")
            raw = buf[pos:pos + lead]
            if len(raw) < lead:
                raise Malformed("literal name runs past end of frame")
            if not set(raw) <= NAME_CHARS:
                raise Malformed("illegal character in name")
            name = raw.decode("ascii")
            pos += lead
        if pos + 2 > len(buf):
            raise Malformed("truncated value length")
        (vlen,) = struct.unpack_from(">H", buf, pos)
        pos += 2
        raw = buf[pos:pos + vlen]
        if len(raw) < vlen:
            raise Malformed("value runs past end of frame")
        if b"\0" in raw:
            raise Malformed("NUL in value")
        try:
            fields.append((name, raw.decode("utf-8")))
        except UnicodeDecodeError:
            raise Malformed("value is not UTF-8")
        pos += vlen
    return fields


def show(frame, arrow):
    """Text for -v: a summary line, decoded headers, then the bytes."""
    wire = frame.encode()
    kind = KIND.get(frame.type)
    if kind:
        lines = ["%s %s len=%d flags=%s stream=%d" % (
            arrow, kind, len(frame.payload), "END_STREAM" if frame.end else "-", frame.stream)]
    else:
        lines = ["%s UNKNOWN(0x%02x) len=%d flags=0x%02x stream=%d" % (
            arrow, frame.type, len(frame.payload), frame.flags, frame.stream)]
    if frame.type == HEADERS:
        try:
            lines += ["    %s: %s" % f for f in decode_fields(frame.payload)]
        except Malformed as e:
            lines.append("    (malformed: %s)" % e)
    shown = wire[:HEADER_LEN + 256]
    for off in range(0, len(shown), 16):
        row = shown[off:off + 16]
        lines.append("  %04x  %-48s |%s|" % (
            off, "".join("%02x " % b for b in row),
            "".join(chr(b) if 32 <= b < 127 else "." for b in row)))
    if len(wire) > len(shown):
        lines.append("  ... %d more bytes" % (len(wire) - len(shown)))
    return "\n".join(lines) + "\n"
