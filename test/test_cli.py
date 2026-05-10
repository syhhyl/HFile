from __future__ import annotations

import os
import socket
import subprocess
import unittest
from pathlib import Path

from test.support.hf import (
    HFileServer,
    make_temp_dir,
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
                "stderr_contains": ["usage:"],
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
                "name": "stop_is_not_a_command",
                "args": ["stop", "-p", "9999"],
                "rc": 1,
                "stderr_contains": ["invalid argument", "usage:"],
            },
            {"name": "g_removed", "args": ["-g", "out"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
            {"name": "m_removed", "args": ["-m", "hello"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
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
            {"name": "o_removed", "args": ["-o", "out.txt"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
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
        os.name != "nt", "POSIX port reuse behavior differs by platform"
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

    def test_client_rejects_directory_source(self) -> None:
        if os.name == "nt":
            self.skipTest("directory open behavior differs on Windows")
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

if __name__ == "__main__":
    unittest.main(verbosity=2)
