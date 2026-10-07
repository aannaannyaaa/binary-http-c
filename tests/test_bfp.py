"""End-to-end tests for the C bserve/bcurl.

Every byte here is packed by hand from SPEC.md -- nothing is shared with
the C code -- so these test the spec, not just C-talks-to-C.

    make && python3 tests/test_bfp.py
"""

import os
import socket
import subprocess
import sys
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WWW = os.path.join(ROOT, "www")
EXE = ".exe" if os.name == "nt" else ""
BSERVE = os.path.join(ROOT, "bserve" + EXE)
BCURL = os.path.join(ROOT, "bcurl" + EXE)

STATIC = [":method", ":path", ":status", ":authority", "user-agent", "accept",
          "content-type", "content-length", "server", "date"]


# ----------------------------------------------------- spec, by hand

def frame(ftype, flags, stream, payload=b""):
    return len(payload).to_bytes(2, "big") + bytes([ftype, flags]) + stream.to_bytes(2, "big") + payload


def field(name, value):
    v = value.encode()
    if name in STATIC:
        head = bytes([0x80 | (STATIC.index(name) + 1)])
    else:
        head = bytes([len(name)]) + name.encode()
    return head + len(v).to_bytes(2, "big") + v


def get(path, stream=1, method="GET"):
    return frame(0x1, 0x1, stream, field(":method", method) + field(":path", path))


def parse_headers(p):
    out, i = {}, 0
    while i < len(p):
        b = p[i]; i += 1
        if b & 0x80:
            name = STATIC[(b & 0x7F) - 1]
        else:
            name = p[i:i + b].decode(); i += b
        n = int.from_bytes(p[i:i + 2], "big"); i += 2
        out[name] = p[i:i + n].decode(); i += n
    return out


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def read_frame(s):
    h = recv_exact(s, 6)
    n = int.from_bytes(h[0:2], "big")
    return h[2], h[3], int.from_bytes(h[4:6], "big"), recv_exact(s, n) if n else b""


def read_response(s):
    headers, body, data_frames = None, b"", []
    while True:
        t, fl, st, p = read_frame(s)
        if t == 0x1:
            headers = parse_headers(p)
        elif t == 0x0:
            body += p
            data_frames.append(len(p))
        if fl & 1:
            return st, headers, body, data_frames


# ------------------------------------------------------------ tests

class BfpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        s = socket.socket(); s.bind(("127.0.0.1", 0))
        cls.port = s.getsockname()[1]; s.close()
        cls.srv = subprocess.Popen([BSERVE, WWW, str(cls.port)], stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", cls.port)).close()
                break
            except OSError:
                time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.srv.terminate(); cls.srv.wait()

    def conn(self):
        s = socket.create_connection(("127.0.0.1", self.port))
        s.settimeout(5)
        self.addCleanup(s.close)
        return s

    def ask(self, raw):
        s = self.conn(); s.sendall(raw)
        return read_response(s)

    def bcurl(self, *args):
        return subprocess.run([BCURL] + list(args), capture_output=True, timeout=10)

    def file(self, name):
        with open(os.path.join(WWW, name), "rb") as f:
            return f.read()

    # server
    def test_200(self):
        st, h, body, _ = self.ask(get("/index.html"))
        self.assertEqual((st, h[":status"]), (1, "200"))
        self.assertEqual(body, self.file("index.html"))
        self.assertEqual(int(h["content-length"]), len(body))

    def test_directory_index(self):
        self.assertEqual(self.ask(get("/"))[1][":status"], "200")

    def test_head_has_no_body(self):
        st, h, body, frames = self.ask(get("/index.html", method="HEAD"))
        self.assertEqual((h[":status"], body, frames), ("200", b"", []))

    def test_404(self):
        self.assertEqual(self.ask(get("/nope"))[1][":status"], "404")

    def test_traversal(self):
        for p in ("/../Makefile", "/%2e%2e/Makefile", "/docs/../../Makefile"):
            self.assertEqual(self.ask(get(p))[1][":status"], "404", p)

    def test_405(self):
        self.assertEqual(self.ask(get("/", method="POST"))[1][":status"], "405")

    def test_400_reserved_index_and_connection_survives(self):
        s = self.conn()
        s.sendall(frame(0x1, 0x1, 1, b"\xff\x00\x00"))
        self.assertEqual(read_response(s)[1][":status"], "400")
        s.sendall(get("/index.html", stream=2))
        st, h, _, _ = read_response(s)
        self.assertEqual((st, h[":status"]), (2, "200"))

    def test_400_truncated_value(self):
        self.assertEqual(self.ask(frame(0x1, 0x1, 1, b"\x81\x00\x09GET"))[1][":status"], "400")

    def test_400_missing_path(self):
        self.assertEqual(self.ask(frame(0x1, 0x1, 1, field(":method", "GET")))[1][":status"], "400")

    def test_400_no_end_stream(self):
        raw = frame(0x1, 0x0, 1, field(":method", "GET") + field(":path", "/"))
        self.assertEqual(self.ask(raw)[1][":status"], "400")

    def test_400_stream_zero(self):
        st, h, _, _ = self.ask(get("/", stream=0))
        self.assertEqual((st, h[":status"]), (0, "400"))

    def test_unknown_frame_type_skipped(self):
        st, h, _, _ = self.ask(frame(0x42, 0xFF, 9, b"something from v2") + get("/index.html"))
        self.assertEqual((st, h[":status"]), (1, "200"))

    def test_many_requests_one_connection(self):
        s = self.conn()
        for i in range(1, 8):
            s.sendall(get("/docs/notes.txt", stream=i))
            st, h, body, _ = read_response(s)
            self.assertEqual((st, h[":status"], body), (i, "200", self.file("docs/notes.txt")))

    def test_big_file_split(self):
        st, h, body, frames = self.ask(get("/big.bin"))
        self.assertEqual(body, self.file("big.bin"))
        self.assertGreater(len(frames), 1)
        self.assertTrue(all(n <= 16384 for n in frames))

    # client
    def test_bcurl_stdout(self):
        r = self.bcurl("localhost:%d/index.html" % self.port)
        self.assertEqual((r.returncode, r.stdout), (0, self.file("index.html")))

    def test_bcurl_many_paths(self):
        r = self.bcurl("localhost:%d/index.html" % self.port, "/docs/notes.txt")
        self.assertEqual(r.stdout, self.file("index.html") + self.file("docs/notes.txt"))

    def test_bcurl_exit_on_404(self):
        self.assertEqual(self.bcurl("localhost:%d/nope" % self.port).returncode, 22)

    def test_bcurl_verbose(self):
        r = self.bcurl("-v", "localhost:%d/index.html" % self.port)
        for needle in (b"> HEADERS", b"< HEADERS", b"< DATA", b"0000  00 "):
            self.assertIn(needle, r.stderr)

    def test_bcurl_one_connection_only(self):
        r = self.bcurl("localhost:%d/" % self.port, "otherhost:1/")
        self.assertEqual(r.returncode, 2)

    def test_bcurl_probe(self):
        self.assertEqual(self.bcurl("--probe", "localhost:%d/" % self.port).returncode, 0)

    def test_bcurl_against_fake_server(self):
        """bcurl must follow the spec, not just bserve: a hand-rolled server
        that sends an unknown frame and splits the body must still work."""
        ls = socket.socket(); ls.bind(("127.0.0.1", 0)); ls.listen(1)
        port = ls.getsockname()[1]
        p = subprocess.Popen([BCURL, "localhost:%d/x" % port], stdout=subprocess.PIPE)
        c, _ = ls.accept()
        t, fl, st, payload = read_frame(c)
        self.assertEqual((t, fl, st), (0x1, 0x1, 1))
        self.assertEqual(parse_headers(payload)[":path"], "/x")
        c.sendall(frame(0x99, 0, 0, b"ignore me")
                  + frame(0x1, 0, 1, field(":status", "200") + field("x-test", "1"))
                  + frame(0x0, 0, 1, b"hel") + frame(0x0, 1, 1, b"lo"))
        out, _ = p.communicate(timeout=5)
        c.close(); ls.close()
        self.assertEqual((p.returncode, out), (0, b"hello"))


if __name__ == "__main__":
    unittest.main(verbosity=1)
