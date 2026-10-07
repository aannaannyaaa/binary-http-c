#!/usr/bin/env python3
"""bcurl.py -- BFP/1 client, Python edition.

    python3 py/bcurl.py [-v] [-I] [--probe] localhost:9000/index.html [/more ...]

Same flags and exit codes as the C bcurl: 0 ok, 22 any 4xx/5xx,
7 cannot connect, 8 bad reply, 2 usage.
"""

import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bfp import (DATA, END_STREAM, GOAWAY, HEADER, HEADER_LEN, HEADERS,  # noqa: E402
                 Frame, Malformed, decode_fields, show)

AGENT = "bcurl-py/1.0"
PROBE_TYPE = 0xFA


class BadReply(Exception):
    pass


def split_url(url):
    if "://" in url:
        url = url.split("://", 1)[1]
    hostport, _, rest = url.partition("/")
    host, _, port = hostport.partition(":")
    if not host or (port and not port.isdigit()):
        raise ValueError("bad URL: " + url)
    return host, int(port or 9000), "/" + rest


class Link:
    """The one and only connection."""

    def __init__(self, host, port, verbose):
        self.sock = socket.create_connection((host, port))
        self.inbox = self.sock.makefile("rb")
        self.verbose = verbose
        self.authority = "%s:%d" % (host, port)
        self.stream = 0

    def send(self, frame):
        if self.verbose:
            sys.stderr.write(show(frame, ">"))
        self.sock.sendall(frame.encode())

    def receive(self):
        head = self.inbox.read(HEADER_LEN)
        if len(head) < HEADER_LEN:
            raise BadReply("server closed the connection mid-response")
        length, ftype, flags, stream = HEADER.unpack(head)
        payload = self.inbox.read(length)
        if len(payload) < length:
            raise BadReply("server closed the connection mid-frame")
        frame = Frame(ftype, flags, stream, payload)
        if self.verbose:
            sys.stderr.write(show(frame, "<"))
        return frame

    def get(self, method, path, out):
        self.stream = self.stream % 0xFFFF + 1
        sid = self.stream
        self.send(Frame.headers(sid, [(":method", method), (":path", path),
                                      (":authority", self.authority),
                                      ("user-agent", AGENT), ("accept", "*/*")], end=True))
        status, declared, got = None, None, 0
        while True:
            frame = self.receive()
            if frame.type == GOAWAY:
                raise BadReply("server sent GOAWAY: " + frame.payload.decode("utf-8", "replace"))
            if frame.type not in (HEADERS, DATA):
                continue                                # SPEC 3.1
            if frame.stream != sid:
                print("bcurl.py: ignoring frame for stream %d" % frame.stream, file=sys.stderr)
                continue
            if frame.type == HEADERS:
                if status is not None:
                    raise BadReply("second HEADERS frame")
                try:
                    fields = dict(decode_fields(frame.payload))
                except Malformed as e:
                    raise BadReply("malformed response headers: %s" % e)
                if ":status" not in fields:
                    raise BadReply("response without :status")
                status = int(fields[":status"])
                declared = fields.get("content-length")
            else:
                if status is None:
                    raise BadReply("DATA before HEADERS")
                out.write(frame.payload)
                got += len(frame.payload)
            if frame.end:
                break
        out.flush()
        if method != "HEAD" and declared is not None and int(declared) != got:
            print("bcurl.py: warning: content-length %s but got %d bytes" % (declared, got),
                  file=sys.stderr)
        if self.verbose:
            print("* %s %s -> %d (%d bytes)" % (method, path, status, got), file=sys.stderr)
        return status

    def close(self):
        try:
            self.send(Frame(GOAWAY, 0, 0, b"bye"))
        except OSError:
            pass
        self.sock.close()


def main(argv):
    verbose = "-v" in argv
    method = "HEAD" if "-I" in argv else "GET"
    probe = "--probe" in argv
    targets = [a for a in argv if a not in ("-v", "-I", "--probe")]
    try:
        host, port, first = split_url(targets[0])
        paths = [first]
        for extra in targets[1:]:
            if extra.startswith("/"):
                paths.append(extra)
                continue
            h, p, path = split_url(extra)
            if (h, p) != (host, port):
                raise ValueError("%s needs a second connection; bcurl never opens one" % extra)
            paths.append(path)
    except (IndexError, ValueError) as e:
        print("bcurl.py: %s" % (e or "usage: bcurl.py [-v] [-I] [--probe] host:port/path"),
              file=sys.stderr)
        return 2

    try:
        link = Link(host, port, verbose)
    except OSError as e:
        print("bcurl.py: cannot connect to %s:%d: %s" % (host, port, e), file=sys.stderr)
        return 7

    code = 0
    try:
        if probe:
            link.send(Frame(PROBE_TYPE, 0, 0, b"hello from v2"))
        for path in paths:
            status = link.get(method, path, sys.stdout.buffer)
            if status >= 400:
                print("bcurl.py: %s -> %d" % (path, status), file=sys.stderr)
                code = 22
    except (BadReply, OSError) as e:
        print("bcurl.py: %s" % e, file=sys.stderr)
        code = 8
    finally:
        link.close()
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
