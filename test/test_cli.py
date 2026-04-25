from __future__ import annotations

import os
import socket
import subprocess
import unittest
from pathlib import Path

from test.support.hf import (
    HFileServer,
    make_temp_dir,
    reserve_free_port,
    resolve_hf_path,
    run_hf,
)


class TestCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            cls.hf_path = resolve_hf_path()
        except FileNotFoundError as e:
            raise unittest.SkipTest(str(e))

    def test_help_prints_usage(self) -> None:
        r = run_hf(self.hf_path, ["-h"], timeout=5.0)
        self.assertEqual(r.returncode, 0, f"argv={r.argv} stderr={r.stderr!r}")
        self.assertIn("usage:", r.stderr)

    def test_invalid_cli_args(self) -> None:
        cases = [
            {
                "name": "no_args",
                "args": [],
                "rc": 1,
                "stderr_contains": [
                    "must specify one of -d, -c, -g, -m, status, or stop",
                    "usage:",
                ],
            },
            {
                "name": "invalid_token",
                "args": ["daemon"],
                "rc": 1,
                "stderr_contains": ["invalid argument", "usage:"],
            },
            {
                "name": "duplicate_d",
                "args": ["-d", "out1", "-d", "out2"],
                "rc": 1,
                "stderr_contains": ["duplicate -d", "usage:"],
            },
            {
                "name": "server_and_client",
                "args": ["-d", "out", "-c", "in"],
                "rc": 1,
                "stderr_contains": [
                    "cannot use server and client modes together",
                    "usage:",
                ],
            },
            {
                "name": "server_and_message",
                "args": ["-d", "out", "-m", "hello"],
                "rc": 1,
                "stderr_contains": [
                    "cannot use server and client modes together",
                    "usage:",
                ],
            },
            {
                "name": "control_with_p",
                "args": ["stop", "-p", "9999"],
                "rc": 1,
                "stderr_contains": ["control mode does not accept -p", "usage:"],
            },
            {
                "name": "put_and_get",
                "args": ["-c", "in", "-g", "out"],
                "rc": 1,
                "stderr_contains": [
                    "must choose exactly one client action: -c, -g, or -m",
                    "usage:",
                ],
            },
            {
                "name": "server_has_i",
                "args": ["-d", "out", "-i", "127.0.0.1"],
                "rc": 1,
                "stderr_contains": ["server mode does not accept -i", "usage:"],
            },
            {
                "name": "invalid_port",
                "args": ["-d", "out", "-p", "nope"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
            },
            {
                "name": "output_requires_get",
                "args": ["-o", "out.txt"],
                "rc": 1,
                "stderr_contains": ["-o requires -g", "usage:"],
            },
        ]

        for c in cases:
            with self.subTest(name=c["name"], args=c["args"]):
                r = run_hf(self.hf_path, c["args"], timeout=5.0)
                self.assertEqual(
                    r.returncode,
                    c["rc"],
                    f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
                )
                for needle in c["stderr_contains"]:
                    self.assertIn(
                        needle,
                        r.stderr,
                        f"argv={r.argv} missing {needle!r} in stderr={r.stderr!r}",
                    )

    def test_client_rejects_invalid_ip_literal(self) -> None:
        with make_temp_dir(prefix="hf_cli_") as tmp_dir:
            src = Path(tmp_dir) / "ip.txt"
            src.write_bytes(b"hello\n")

            r = run_hf(
                self.hf_path,
                ["-c", src, "-i", "not_an_ip"],
                timeout=5.0,
            )

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("inet_pton", r.stderr)

    def test_dash_prefixed_values_are_not_rejected_by_parser(self) -> None:
        cases = [
            {
                "name": "message",
                "args": ["-m", "-hello", "-i", "not_an_ip"],
                "not_contains": "invalid message",
            },
            {
                "name": "remote_file",
                "args": ["-g", "-remote.txt", "-i", "not_an_ip"],
                "not_contains": "invalid remote file",
            },
            {
                "name": "output_path",
                "args": ["-g", "remote.txt", "-o", "-out.txt", "-i", "not_an_ip"],
                "not_contains": "invalid output path",
            },
        ]

        for c in cases:
            with self.subTest(name=c["name"]):
                r = run_hf(self.hf_path, c["args"], timeout=5.0)
                self.assertEqual(
                    r.returncode,
                    1,
                    f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
                )
                self.assertIn("inet_pton", r.stderr)
                self.assertNotIn(c["not_contains"], r.stderr)

    def test_client_rejects_missing_source_file(self) -> None:
        with make_temp_dir(prefix="hf_cli_") as tmp_dir:
            missing = Path(tmp_dir) / "missing.txt"
            r = run_hf(self.hf_path, ["-c", missing], timeout=5.0)

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("open", r.stderr)

    @unittest.skipIf(
        os.name != "nt", "POSIX daemon parent does not expose child shutdown state"
    )
    def test_server_start_failure_is_not_reported_as_signal_exit(self) -> None:
        with make_temp_dir(prefix="hf_cli_bind_") as tmp_dir:
            out_dir = Path(tmp_dir) / "out"
            out_dir.mkdir()

            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                    listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                port = listener.getsockname()[1]

                r = run_hf(
                    self.hf_path, ["-d", out_dir, "-p", str(port)], timeout=10.0
                )
            except subprocess.TimeoutExpired:
                try:
                    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    sock.settimeout(1.0)
                    sock.connect(("127.0.0.1", port))
                    sock.close()
                    self.fail(
                        f"server is running on port {port} - "
                        "SO_EXCLUSIVEADDRUSE failed to prevent rebind; "
                        "test scenario requires Windows exclusive port binding"
                    )
                except ConnectionRefusedError:
                    self.fail(
                        "server process timed out but port is not accepting connections; "
                        "bind may have failed but process did not exit properly"
                    )
                except OSError:
                    self.fail(
                        "server process did not exit within 10s after bind failure"
                    )
            finally:
                listener.close()

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )

    @unittest.skipIf(os.name == "nt", "directory open behavior differs on Windows")
    def test_client_rejects_directory_source(self) -> None:
        with make_temp_dir(prefix="hf_cli_") as tmp_dir:
            src_dir = Path(tmp_dir) / "source-dir"
            src_dir.mkdir()
            r = run_hf(self.hf_path, ["-c", src_dir], timeout=5.0)

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("invalid source file", r.stderr)

    def test_get_rejects_invalid_remote_file(self) -> None:
        r = run_hf(self.hf_path, ["-g", "../bad"], timeout=5.0)
        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("invalid remote file", r.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX daemon state path coverage")
    def test_stop_cleans_stale_daemon_state(self) -> None:
        state_path = Path(os.environ.get("TMPDIR") or "/tmp") / "hf-daemon.state"
        original = state_path.read_bytes() if state_path.exists() else None

        try:
            stale_pid = 999999
            while stale_pid > 1:
                try:
                    os.kill(stale_pid, 0)
                    stale_pid -= 1
                except ProcessLookupError:
                    break
                except PermissionError:
                    stale_pid -= 1
            state_path.write_text(
                f"pid={stale_pid}\n"
                f"receive_dir=/tmp\n"
                f"port=8888\n"
                f"web_url=http://127.0.0.1:8888/\n"
                f"log_path=/tmp/hf-daemon.log\n"
                f"daemon_mode=1\n",
                encoding="utf-8",
            )

            r = run_hf(self.hf_path, ["stop"], timeout=5.0)

            self.assertEqual(
                r.returncode,
                0,
                f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
            )
            self.assertIn("already stopped", r.stdout)
            self.assertFalse(state_path.exists())
        finally:
            if original is not None:
                state_path.write_bytes(original)
            else:
                state_path.unlink(missing_ok=True)

    @unittest.skipIf(os.name == "nt", "POSIX daemon state path coverage")
    def test_status_cleans_malformed_daemon_state(self) -> None:
        state_path = Path(os.environ.get("TMPDIR") or "/tmp") / "hf-daemon.state"
        original = state_path.read_bytes() if state_path.exists() else None

        try:
            state_path.write_text(f"pid={os.getpid()}\n", encoding="utf-8")

            r = run_hf(self.hf_path, ["status"], timeout=5.0)

            self.assertEqual(
                r.returncode,
                1,
                f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
            )
            self.assertIn("No running daemon", r.stdout)
            self.assertFalse(state_path.exists())
        finally:
            if original is not None:
                state_path.write_bytes(original)
            else:
                state_path.unlink(missing_ok=True)

    @unittest.skipIf(os.name == "nt", "Windows does not daemonize")
    def test_daemon_rejects_second_instance_on_different_port(self) -> None:
        with make_temp_dir(prefix="hf_cli_daemon_") as tmp_dir:
            base_dir = Path(tmp_dir)
            first_dir = base_dir / "first"
            second_dir = base_dir / "second"
            server = HFileServer(
                hf_path=self.hf_path,
                out_dir=first_dir,
                port=reserve_free_port(),
            )
            server.start(startup_timeout=5.0)
            try:
                second = run_hf(
                    self.hf_path,
                    ["-d", second_dir, "-p", str(reserve_free_port())],
                    timeout=5.0,
                )
                self.assertEqual(
                    1,
                    second.returncode,
                    f"argv={second.argv} stdout={second.stdout!r} stderr={second.stderr!r}",
                )
                self.assertIn("HFile is already running", second.stderr)
            finally:
                server.stop()

if __name__ == "__main__":
    unittest.main(verbosity=2)
