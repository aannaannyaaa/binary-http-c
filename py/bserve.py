#!/usr/bin/env python3
"""bserve.py -- BFP/1 file server, Python/asyncio edition.

    python3 py/bserve.py ./www 9000 [-v]

Speaks exactly the same bytes as the C bserve (see SPEC.md).
"""

import asyncio
import mimetypes
import os
import sys
import urllib.parse
from email.utils import formatdate
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bfp import (DATA, DATA_CHUNK, END_STREAM, GOAWAY, HEADER, HEADER_LEN,  # noqa: E402
                 HEADERS, Frame, Malformed, decode_fields, show)

SERVER = "bserve-py/1.0"
IDLE_SECONDS = 120


class Session:
    def __init__(self, root, reader, writer, verbose):
        self.root = root
        self.reader = reader
        self.writer = writer
        self.verbose = verbose
        self.peer = "%s:%d" % writer.get_extra_info("peername")[:2]
        self.rejected = None

    def log(self, text):
        print("[%s] %s" % (self.peer, text), file=sys.stderr, flush=True)

    async def send(self, frame):
        if self.verbose:
            sys.stderr.write(show(frame, ">"))
        self.writer.write(frame.encode())
        await self.writer.drain()

    async def reply_head(self, stream, status, ctype, length, extra=(), end=False):
        fields = [(":status", str(status)), ("server", SERVER),
                  ("date", formatdate(usegmt=True)), ("content-type", ctype),
                  ("content-length", str(length)), *extra]
        await self.send(Frame.headers(stream, fields, end))

    async def fail(self, stream, status, why):
        self.log("#%d -> %d %s" % (stream, status, why))
        self.rejected = stream
        body = ("%d %s\n" % (status, why)).encode()
        await self.reply_head(stream, status, "text/plain; charset=utf-8", len(body))
        await self.send(Frame(DATA, END_STREAM, stream, body))

    def locate(self, path):
        """:path -> Path under root, or None. Same rules as the C server."""
        rel = urllib.parse.unquote(path.split("?", 1)[0])
        if "\0" in rel or "\\" in rel or ":" in rel:
            return None
        if ".." in rel.split("/"):
            return None
        target = self.root / rel.lstrip("/")
        if target.is_dir():
            target = target / "index.html"
        return target if target.is_file() else None

    async def on_request(self, frame):
        sid = frame.stream
        if sid == 0:
            return await self.fail(0, 400, "HEADERS on stream 0")
        try:
            fields = decode_fields(frame.payload)
        except Malformed as e:
            return await self.fail(sid, 400, str(e))
        if not frame.end:
            return await self.fail(sid, 400, "request HEADERS must set END_STREAM")
        methods = [v for k, v in fields if k == ":method"]
        paths = [v for k, v in fields if k == ":path"]
        if len(methods) != 1 or len(paths) != 1:
            return await self.fail(sid, 400, "need exactly one :method and one :path")
        method, path = methods[0], paths[0]
        if not path.startswith("/"):
            return await self.fail(sid, 400, ":path must start with /")
        if method not in ("GET", "HEAD"):
            return await self.fail(sid, 405, "method not allowed")

        target = self.locate(path)
        if target is None:
            return await self.fail(sid, 404, "not found: " + path)

        size = target.stat().st_size
        ctype = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        if ctype.startswith("text/"):
            ctype += "; charset=utf-8"
        modified = formatdate(target.stat().st_mtime, usegmt=True)
        self.log("#%d %s %s -> 200 (%d bytes)" % (sid, method, path, size))

        no_body = method == "HEAD" or size == 0
        await self.reply_head(sid, 200, ctype, size, [("last-modified", modified)], no_body)
        if no_body:
            return
        with target.open("rb") as fh:
            chunk = fh.read(DATA_CHUNK)
            while chunk:
                following = fh.read(DATA_CHUNK)
                await self.send(Frame(DATA, 0 if following else END_STREAM, sid, chunk))
                chunk = following

    async def run(self):
        self.log("connected")
        try:
            while True:
                try:
                    head = await asyncio.wait_for(self.reader.readexactly(HEADER_LEN), IDLE_SECONDS)
                    length, ftype, flags, stream = HEADER.unpack(head)
                    payload = await self.reader.readexactly(length)
                except asyncio.IncompleteReadError as e:
                    if e.partial:
                        self.log("dropped mid-frame")
                    break
                except asyncio.TimeoutError:
                    await self.send(Frame(GOAWAY, 0, 0, b"idle"))
                    break
                frame = Frame(ftype, flags, stream, payload)
                if self.verbose:
                    sys.stderr.write(show(frame, "<"))

                if ftype == HEADERS:
                    await self.on_request(frame)
                elif ftype == DATA:
                    if stream != self.rejected:
                        await self.fail(stream, 400, "unexpected DATA from client")
                elif ftype == GOAWAY:
                    self.log("client sent GOAWAY")
                    break
                else:
                    # SPEC 3.1: payload is already read -- just move on
                    self.log("skipped unknown frame type 0x%02x (%d bytes)" % (ftype, length))
        except ConnectionError as e:
            self.log("connection error: %s" % e)
        finally:
            self.writer.close()
            self.log("closed")


async def main(argv):
    verbose = "-v" in argv
    args = [a for a in argv if a != "-v"]
    if len(args) != 2 or not args[1].isdigit():
        sys.exit("usage: bserve.py ROOT PORT [-v]")
    root = Path(args[0]).resolve()
    if not root.is_dir():
        sys.exit("bserve.py: %s is not a directory" % args[0])

    async def accept(reader, writer):
        await Session(root, reader, writer, verbose).run()

    server = await asyncio.start_server(accept, port=int(args[1]), reuse_address=True)
    print("bserve.py: serving %s on port %s" % (root, args[1]), file=sys.stderr, flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main(sys.argv[1:]))
    except KeyboardInterrupt:
        pass
