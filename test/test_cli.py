from __future__ import annotations

import unittest
from pathlib import Path

from test.support.hf import make_temp_dir, resolve_hf_path, run_hf


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
                "stderr_contains": ["must specify one of -s, -c, or -m", "usage:"],
            },
            {
                "name": "no_mode_with_ip",
                "args": ["-i", "10.0.0.1"],
                "rc": 1,
                "stderr_contains": [
                    "must specify one of -s, -c, or -m",
                    "usage:",
                ],
            },
            {
                "name": "no_mode_with_perf",
                "args": ["--perf"],
                "rc": 1,
                "stderr_contains": [
                    "must specify one of -s, -c, or -m",
                    "usage:",
                ],
            },
            {
                "name": "invalid_token",
                "args": ["a"],
                "rc": 1,
                "stderr_contains": ["invalid argument", "usage:"],
            },
            {
                "name": "unknown_flag",
                "args": ["-z"],
                "rc": 1,
                "stderr_contains": ["usage:"],
            },
            {
                "name": "mutual_exclusion_c_then_s",
                "args": ["-c", "in", "-s", "out"],
                "rc": 1,
                "stderr_contains": ["cannot use -s -c together", "usage:"],
            },
            {
                "name": "mutual_exclusion_s_then_c",
                "args": ["-s", "out", "-c", "in"],
                "rc": 1,
                "stderr_contains": ["cannot use -s -c together", "usage:"],
            },
            {
                "name": "duplicate_server_mode",
                "args": ["-s", "out1", "-s", "out2"],
                "rc": 1,
                "stderr_contains": ["duplicate -s", "usage:"],
            },
            {
                "name": "server_path_missing",
                "args": ["-s"],
                "rc": 1,
                "stderr_contains": ["invalid server path", "usage:"],
            },
            {
                "name": "server_path_is_flag",
                "args": ["-s", "-c"],
                "rc": 1,
                "stderr_contains": ["invalid server path", "usage:"],
            },
            {
                "name": "client_path_missing",
                "args": ["-c"],
                "rc": 1,
                "stderr_contains": ["invalid client path", "usage:"],
            },
            {
                "name": "client_path_is_flag",
                "args": ["-c", "-s"],
                "rc": 1,
                "stderr_contains": ["invalid client path", "usage:"],
            },
            {
                "name": "duplicate_client_mode",
                "args": ["-c", "in1", "-c", "in2"],
                "rc": 1,
                "stderr_contains": ["duplicate -c", "usage:"],
            },
            {
                "name": "message_missing",
                "args": ["-m"],
                "rc": 1,
                "stderr_contains": ["invalid message", "usage:"],
            },
            {
                "name": "duplicate_message_mode",
                "args": ["-m", "hello", "-m", "world"],
                "rc": 1,
                "stderr_contains": ["duplicate -m", "usage:"],
            },
            {
                "name": "mutual_exclusion_c_then_m",
                "args": ["-c", "in", "-m", "hello"],
                "rc": 1,
                "stderr_contains": ["cannot use -c -m together", "usage:"],
            },
            {
                "name": "mutual_exclusion_s_then_m",
                "args": ["-s", "out", "-m", "hello"],
                "rc": 1,
                "stderr_contains": ["cannot use -s -m together", "usage:"],
            },
            {
                "name": "message_mode_has_compress",
                "args": ["-m", "hello", "--compress"],
                "rc": 1,
                "stderr_contains": [
                    "message mode does not accept --compress",
                    "usage:",
                ],
            },
            {
                "name": "server_mode_has_ip",
                "args": ["-s", "out", "-i", "10.0.0.1"],
                "rc": 1,
                "stderr_contains": ["server mode does not accept -i", "usage:"],
            },
            {
                "name": "port_missing_value",
                "args": ["-s", "out", "-p"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
            },
            {
                "name": "port_not_number_server",
                "args": ["-s", "out", "-p", "hfile"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
            },
            {
                "name": "port_not_number_client",
                "args": ["-c", "in", "-p", "hfile"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
            },
            {
                "name": "port_zero",
                "args": ["-s", "out", "-p", "0"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
            },
            {
                "name": "port_too_large",
                "args": ["-s", "out", "-p", "65536"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
            },
            {
                "name": "ip_missing_value",
                "args": ["-c", "in", "-i"],
                "rc": 1,
                "stderr_contains": ["invalid argument", "usage:"],
            },
            {
                "name": "ip_value_is_flag",
                "args": ["-c", "in", "-i", "-p"],
                "rc": 1,
                "stderr_contains": ["invalid argument", "usage:"],
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
