from __future__ import annotations

import unittest
from pathlib import Path

from test.support.hf import (
    HFileServer,
    make_temp_dir,
    resolve_hf_path,
    run_hf,
    wait_for_text_in_file,
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
                "name": "duplicate_s",
                "args": ["-s", "f1", "-s", "f2"],
                "rc": 1,
                "stderr_contains": ["duplicate -s", "usage:"],
            },
            {
                "name": "server_and_client",
                "args": ["/tmp", "-s", "in"],
                "rc": 1,
                "stderr_contains": [
                    "cannot use server path with -s",
                    "usage:",
                ],
            },
            {
                "name": "extra_positional",
                "args": ["/tmp", "/tmp2"],
                "rc": 1,
                "stderr_contains": ["unexpected extra argument", "usage:"],
            },
            {"name": "g_removed", "args": ["-g", "out"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
            {"name": "m_removed", "args": ["-m", "hello"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
            {
                "name": "server_has_i",
                "args": ["/tmp", "-i", "127.0.0.1"],
                "rc": 1,
                "stderr_contains": ["server mode does not accept -i", "usage:"],
            },
            {
                "name": "invalid_port",
                "args": ["/tmp", "-p", "nope"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
            },
            {"name": "o_removed", "args": ["-o", "out.txt"], "rc": 1, "stderr_contains": ["invalid argument", "usage:"]},
            {"name": "client_q", "args": ["-s", "in", "-q"], "rc": 1, "stderr_contains": ["client mode does not accept -q", "usage:"]},
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
                ["-s", src, "-i", "not_an_ip"],
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
            r = run_hf(self.hf_path, ["-s", missing], timeout=5.0)

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("open", r.stderr)

    def test_client_rejects_directory_source(self) -> None:
        with make_temp_dir(prefix="hf_cli_") as tmp_dir:
            src_dir = Path(tmp_dir) / "source-dir"
            src_dir.mkdir()
            r = run_hf(self.hf_path, ["-s", src_dir], timeout=5.0)

        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("invalid source file", r.stderr)

    def test_server_q_prints_connect_url_and_qr(self) -> None:
        with make_temp_dir(prefix="hf_cli_") as tmp_dir:
            out_dir = Path(tmp_dir) / "recv"
            log_path = Path(tmp_dir) / "server.log"
            server = HFileServer(
                hf_path=self.hf_path,
                out_dir=out_dir,
                log_path=log_path,
                extra_args=["-q"],
            )
            try:
                server.start()
                text = wait_for_text_in_file(log_path, "Connect URL", timeout=5.0)
                self.assertIsNotNone(text)
                assert text is not None
                self.assertIn(f"hfile://", text)
                self.assertIn(f":{server.port}/", text)
                self.assertTrue(any(ch in text for ch in "█▀▄"))
            finally:
                server.stop()

if __name__ == "__main__":
    unittest.main(verbosity=2)
