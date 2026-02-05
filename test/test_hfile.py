import os
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from typing import Optional


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def _bin_path() -> str:
    # Prefer local build, then installed binary; allow override.
    env = os.environ.get("HFILE_BIN")
    if env:
        return env
    local = os.path.join(REPO_ROOT, "build", "hf")
    if os.path.exists(local):
        return local
    return "hf"


def _get_free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _wait_port(ip: str, port: int, timeout_s: float = 2.0) -> None:
    deadline = time.time() + timeout_s
    while True:
        try:
            sock = socket.create_connection((ip, port), timeout=0.1)
            sock.close()
            return
        except OSError:
            if time.time() >= deadline:
                raise TimeoutError(f"server did not listen on {ip}:{port}")
            time.sleep(0.02)


class ServerProc:
    def __init__(self, save_dir: str, port: int):
        self.save_dir = save_dir
        self.port = port
        self.proc: Optional[subprocess.Popen] = None

    def start(self):
        hf = _bin_path()
        self.proc = subprocess.Popen(
            [hf, "-s", self.save_dir, "-p", str(self.port)],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        _wait_port("127.0.0.1", self.port)
        return self

    def stop(self):
        if self.proc is None:
            return
        proc = self.proc
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=1.5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=1.5)

        if proc.stdout is not None:
            proc.stdout.close()
        if proc.stderr is not None:
            proc.stderr.close()


def _client_send(file_path: str, ip: str, port: int) -> subprocess.CompletedProcess:
    hf = _bin_path()
    return subprocess.run(
        [hf, "-c", file_path, "-i", ip, "-p", str(port)],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )


def _raw_send(ip: str, port: int, name_bytes: bytes, content: bytes = b"hello") -> None:
    sock = socket.create_connection((ip, port), timeout=1.0)
    pkt = struct.pack("!H", len(name_bytes)) + name_bytes + content
    sock.sendall(pkt)
    sock.shutdown(socket.SHUT_WR)
    sock.close()


def _raw_send_truncated_name(
    ip: str, port: int, claimed_len: int, sent_name: bytes
) -> None:
    sock = socket.create_connection((ip, port), timeout=1.0)
    sock.sendall(struct.pack("!H", claimed_len))
    if sent_name:
        sock.sendall(sent_name)
    sock.close()


class TestHFileIntegration(unittest.TestCase):
    def test_basic_file_send(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            in_file = os.path.join(tmp, "in.txt")
            payload = b"hello\nworld\n"
            with open(in_file, "wb") as f:
                f.write(payload)

            port = _get_free_port()
            srv = ServerProc(out_dir, port).start()
            try:
                cp = _client_send(in_file, "127.0.0.1", port)
                self.assertEqual(cp.returncode, 0, cp.stderr)

                saved = os.path.join(out_dir, "in.txt")
                self.assertTrue(os.path.exists(saved))
                with open(saved, "rb") as f:
                    self.assertEqual(f.read(), payload)
            finally:
                srv.stop()

    def test_overwrite_existing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            port = _get_free_port()
            srv = ServerProc(out_dir, port).start()
            try:
                in_file = os.path.join(tmp, "same.txt")
                with open(in_file, "wb") as f:
                    f.write(b"new")
                existing = os.path.join(out_dir, "same.txt")
                with open(existing, "wb") as f:
                    f.write(b"old")

                cp = _client_send(in_file, "127.0.0.1", port)
                self.assertEqual(cp.returncode, 0, cp.stderr)
                with open(existing, "rb") as f:
                    self.assertEqual(f.read(), b"new")
            finally:
                srv.stop()

    def test_multiple_sends_same_server(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            port = _get_free_port()
            srv = ServerProc(out_dir, port).start()
            try:
                f1 = os.path.join(tmp, "a.txt")
                f2 = os.path.join(tmp, "b.txt")
                with open(f1, "wb") as f:
                    f.write(b"A")
                with open(f2, "wb") as f:
                    f.write(b"B")

                cp1 = _client_send(f1, "127.0.0.1", port)
                self.assertEqual(cp1.returncode, 0, cp1.stderr)
                cp2 = _client_send(f2, "127.0.0.1", port)
                self.assertEqual(cp2.returncode, 0, cp2.stderr)

                with open(os.path.join(out_dir, "a.txt"), "rb") as f:
                    self.assertEqual(f.read(), b"A")
                with open(os.path.join(out_dir, "b.txt"), "rb") as f:
                    self.assertEqual(f.read(), b"B")
            finally:
                srv.stop()

    def test_filename_with_spaces(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            in_file = os.path.join(tmp, "a b.txt")
            with open(in_file, "wb") as f:
                f.write(b"space")

            port = _get_free_port()
            srv = ServerProc(out_dir, port).start()
            try:
                cp = _client_send(in_file, "127.0.0.1", port)
                self.assertEqual(cp.returncode, 0, cp.stderr)
                self.assertTrue(os.path.exists(os.path.join(out_dir, "a b.txt")))
            finally:
                srv.stop()

    def test_reject_path_traversal(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            port = _get_free_port()
            srv = ServerProc(out_dir, port).start()
            try:
                _raw_send("127.0.0.1", port, b"../pwned.txt", b"x")
                time.sleep(0.05)
                self.assertEqual(os.listdir(out_dir), [])
            finally:
                srv.stop()

    def test_reject_separator_and_dotdot(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            port = _get_free_port()
            srv = ServerProc(out_dir, port).start()
            try:
                for name in [
                    b"a/b.txt",
                    b"a\\b.txt",
                    b"..",
                    b"a..b",
                    b"a/..",
                    b"a..\\b",
                ]:
                    _raw_send("127.0.0.1", port, name, b"x")
                time.sleep(0.05)
                self.assertEqual(os.listdir(out_dir), [])
            finally:
                srv.stop()

    def test_reject_too_long_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            port = _get_free_port()
            srv = ServerProc(out_dir, port).start()
            try:
                _raw_send("127.0.0.1", port, b"a" * 256, b"x")
                time.sleep(0.05)
                self.assertEqual(os.listdir(out_dir), [])
            finally:
                srv.stop()

    def test_truncated_name_does_not_create_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            port = _get_free_port()
            srv = ServerProc(out_dir, port).start()
            try:
                _raw_send_truncated_name(
                    "127.0.0.1", port, claimed_len=10, sent_name=b"abc"
                )
                time.sleep(0.05)
                assert srv.proc is not None
                self.assertIsNone(srv.proc.poll())
                self.assertEqual(os.listdir(out_dir), [])
            finally:
                srv.stop()

    def test_invalid_port_arg(self):
        hf = _bin_path()
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = os.path.join(tmp, "out")
            os.mkdir(out_dir)
            cp = subprocess.run(
                [hf, "-s", out_dir, "-p", "0"],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(cp.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
