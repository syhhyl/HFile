from __future__ import annotations

import os
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

    def test_get_rejects_invalid_remote_file(self) -> None:
        r = run_hf(self.hf_path, ["-g", "../bad"], timeout=5.0)
        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("invalid remote file", r.stderr)

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
