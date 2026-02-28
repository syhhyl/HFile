from __future__ import annotations

import unittest

from util_hf import resolve_hf_path, run_hf


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
                "name": "server_mode_has_ip",
                "args": ["-s", "out", "-i", "10.0.0.1"],
                "rc": 1,
                "stderr_contains": ["server mode don't need ip", "usage:"],
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
