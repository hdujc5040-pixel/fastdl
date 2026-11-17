"""Integration tests for fastdl.exe against a local test server. Run: python tests/run_tests.py"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "..", "fastdl.exe")
SERVER = os.path.join(HERE, "range_server.py")
PERIOD = 251
PATTERN = bytes(range(PERIOD))
failures = []


def expected(offset, length):
    start = offset % PERIOD
    return (PATTERN * ((start + length) // PERIOD + 2))[start:start + length]


def verify(path, size):
    if not os.path.exists(path):
        return "missing file"
    if os.path.getsize(path) != size:
        return "size %d != %d" % (os.path.getsize(path), size)
    with open(path, "rb") as f:
        off = 0
        while off < size:
            data = f.read(1024 * 1024)
            if data != expected(off, len(data)):
                return "content differs near offset %d" % off
            off += len(data)
    return None


class Server:
    def __init__(self, port, size_mb, no_range=False, throttle_kb=0):
        args = [sys.executable, "-I", SERVER, "--port", str(port), "--size-mb", str(size_mb)]
        if no_range:
            args.append("--no-range")
        if throttle_kb:
            args += ["--throttle-kb", str(throttle_kb)]
        self.p = subprocess.Popen(args)
        self.port = port
        time.sleep(0.8)

    def url(self, path="/file.bin"):
        return "http://127.0.0.1:%d%s" % (self.port, path)

    def stop(self):
        self.p.kill()
        self.p.wait()


def run(args, cwd, timeout=120):
    return subprocess.run([EXE] + args, cwd=cwd, capture_output=True, timeout=timeout)


def text(proc):
    return (proc.stdout or b"").decode("utf-8", "replace")


def check(name, ok, detail=""):
    print(("PASS  " if ok else "FAIL  ") + name + ("  - " + detail if detail and not ok else ""))
    if not ok:
        failures.append(name)


def main():
    work = tempfile.mkdtemp(prefix="fastdl_test_")
    try:
        # 1. ranged multi-connection download
        s = Server(8765, 64)
        r = run([s.url(), "-o", "a.bin", "-t", "16"], work)
        check("ranged download 64 MB / 16 connections", r.returncode == 0 and verify(os.path.join(work, "a.bin"), 64 * 1024 * 1024) is None,
              text(r)[-300:] + str(verify(os.path.join(work, "a.bin"), 64 * 1024 * 1024)))
        check("no leftover state file", not os.path.exists(os.path.join(work, "a.bin.fdl")))

        # 2. already complete -> nothing to do
        r = run([s.url(), "-o", "a.bin"], work)
        check("existing complete file is skipped", r.returncode == 0 and "nothing to do" in text(r), text(r)[-200:])

        # 3. name from the url (percent-decoded) after a redirect
        r = run([s.url("/redir"), "-t", "4"], work)
        check("redirect + name from url", r.returncode == 0 and os.path.exists(os.path.join(work, "file name.bin")), text(r)[-300:])
        check("redirected file content", verify(os.path.join(work, "file name.bin"), 64 * 1024 * 1024) is None)

        # 4. name from Content-Disposition
        r = run([s.url("/cd"), "-t", "4"], work)
        check("name from Content-Disposition", r.returncode == 0 and os.path.exists(os.path.join(work, "from_header.bin")), text(r)[-300:])

        # 5. a different file under an existing name gets (1)
        with open(os.path.join(work, "other.bin"), "wb") as f:
            f.write(b"x" * 10)
        r = run([s.url(), "-o", "other.bin", "-t", "4"], work)
        check("existing different file is not overwritten", r.returncode == 0 and os.path.exists(os.path.join(work, "other (1).bin")) and os.path.getsize(os.path.join(work, "other.bin")) == 10, text(r)[-300:])

        # 6. 404
        r = run([s.url("/nope")], work)
        check("404 is reported", r.returncode != 0 and "404" in text(r), text(r)[-200:])
        s.stop()

        # 7. server without Range support
        s = Server(8766, 24, no_range=True)
        r = run([s.url(), "-o", "norange.bin", "-t", "8"], work)
        check("server without Range -> single connection", r.returncode == 0 and "single connection" in text(r) and verify(os.path.join(work, "norange.bin"), 24 * 1024 * 1024) is None, text(r)[-300:])
        s.stop()

        # 8. speed-up from parallel connections on a throttled server (400 KB/s per connection)
        s = Server(8767, 16, throttle_kb=400)
        t = time.time()
        r1 = run([s.url(), "-o", "t1.bin", "-t", "1"], work, timeout=300)
        d1 = time.time() - t
        t = time.time()
        r16 = run([s.url(), "-o", "t16.bin", "-t", "16"], work, timeout=300)
        d16 = time.time() - t
        check("throttled: 1 connection ok", r1.returncode == 0 and verify(os.path.join(work, "t1.bin"), 16 * 1024 * 1024) is None)
        check("throttled: 16 connections ok", r16.returncode == 0 and verify(os.path.join(work, "t16.bin"), 16 * 1024 * 1024) is None)
        check("16 connections are at least 5x faster (%.1fs vs %.1fs)" % (d16, d1), d1 / d16 > 5, "%.1fs vs %.1fs" % (d1, d16))

        s.stop()

        # 9. resume after the process is killed mid-download (32 MB at ~1.6 MB/s, killed after 5 s)
        s = Server(8768, 32, throttle_kb=400)
        p = subprocess.Popen([EXE, s.url(), "-o", "resume.bin", "-t", "4"], cwd=work, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(5)
        p.kill()
        p.wait()
        had_state = os.path.exists(os.path.join(work, "resume.bin.fdl"))
        r = run([s.url(), "-o", "resume.bin", "-t", "4"], work, timeout=300)
        out = text(r)
        check("resume after kill (state file was written)", had_state, "no resume.bin.fdl after 5 s")
        check("resume after kill (continues and verifies)", r.returncode == 0 and "Resume:" in out and verify(os.path.join(work, "resume.bin"), 32 * 1024 * 1024) is None, out[-400:])
        s.stop()
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print()
    if failures:
        print("%d test(s) failed: %s" % (len(failures), ", ".join(failures)))
        sys.exit(1)
    print("all tests passed")


if __name__ == "__main__":
    main()
