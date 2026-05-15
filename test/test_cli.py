from __future__ import annotations

import unittest
from pathlib import Path

from test.support.hf import (
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
                "name": "missing_command",
                "args": [],
                "rc": 1,
                "stderr_contains": ["missing command", "usage:"],
            },
            {
                "name": "unknown_command",
                "args": ["-s", "in"],
                "rc": 1,
                "stderr_contains": ["unknown command", "usage:"],
            },
            {
                "name": "extra_positional",
                "args": ["recv", "/tmp", "/tmp2"],
                "rc": 1,
                "stderr_contains": ["unexpected extra argument", "usage:"],
            },
            {"name": "g_removed", "args": ["recv", "-g", "out"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
            {"name": "m_removed", "args": ["send", "-m", "hello"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
            {
                "name": "recv_has_i",
                "args": ["recv", "/tmp", "-i", "127.0.0.1"],
                "rc": 1,
                "stderr_contains": ["recv mode does not accept -i", "usage:"],
            },
            {
                "name": "invalid_port",
                "args": ["recv", "/tmp", "-p", "nope"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
            },
            {"name": "o_removed", "args": ["send", "-o", "out.txt"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
            {"name": "q_removed", "args": ["recv", "-q"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
            {"name": "send_missing_file", "args": ["send"], "rc": 1, "stderr_contains": ["missing file to send", "usage:"]},
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

    def test_send_rejects_invalid_ip_literal(self) -> None:
        with make_temp_dir(prefix="hf_cli_") as tmp_dir:
            src = Path(tmp_dir) / "ip.txt"
            src.write_bytes(b"hello\n")

            r = run_hf(
                self.hf_path,
                ["send", src, "-i", "not_an_ip"],
                timeout=5.0,
            )

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )

    def test_send_rejects_missing_source_file(self) -> None:
        with make_temp_dir(prefix="hf_cli_") as tmp_dir:
            missing = Path(tmp_dir) / "missing.txt"
            r = run_hf(self.hf_path, ["send", missing], timeout=5.0)

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )

    def test_send_rejects_directory_source(self) -> None:
        with make_temp_dir(prefix="hf_cli_") as tmp_dir:
            src_dir = Path(tmp_dir) / "source-dir"
            src_dir.mkdir()
            r = run_hf(self.hf_path, ["send", src_dir], timeout=5.0)

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )

if __name__ == "__main__":
    unittest.main(verbosity=2)
