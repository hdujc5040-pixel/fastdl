"""Tiny HTTP test server for fastdl: serves a generated file (byte i == i % 251) with optional Range support,
per-connection throttling, a redirect and a Content-Disposition name."""
import argparse
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

PERIOD = 251
PATTERN = bytes(range(PERIOD))


def block(offset, length):
    start = offset % PERIOD
    reps = (start + length) // PERIOD + 2
    return (PATTERN * reps)[start:start + length]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    size = 0
    ranges = True
    throttle = 0  # bytes per second per connection, 0 = unlimited

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = unquote(self.path.split("?")[0])
        if path == "/redir":
            self.send_response(302)
            self.send_header("Location", "/real/file%20name.bin")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if not path.startswith("/real/") and path != "/file.bin" and path != "/cd":
            self.send_error(404)
            return

        total = self.size
        start, end, status = 0, total - 1, 200
        header = self.headers.get("Range")
        if header and self.ranges:
            m = re.match(r"bytes=(\d+)-(\d*)", header)
            if m:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else total - 1
                end = min(end, total - 1)
                status = 206
                if start >= total:
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % total)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return

        length = end - start + 1
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        if self.ranges:
            self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, total))
        if path == "/cd":
            self.send_header("Content-Disposition", 'attachment; filename="from_header.bin"')
        self.end_headers()

        sent, step = 0, 64 * 1024
        t0 = time.time()
        try:
            while sent < length:
                n = min(step, length - sent)
                self.wfile.write(block(start + sent, n))
                sent += n
                if self.throttle:
                    wait = sent / self.throttle - (time.time() - t0)
                    if wait > 0:
                        time.sleep(wait)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--size-mb", type=float, default=64)
    ap.add_argument("--no-range", action="store_true")
    ap.add_argument("--throttle-kb", type=int, default=0)
    a = ap.parse_args()
    Handler.size = int(a.size_mb * 1024 * 1024)
    Handler.ranges = not a.no_range
    Handler.throttle = a.throttle_kb * 1024
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
