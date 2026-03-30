from __future__ import annotations

import http.client
import os
import urllib.request
import unittest
from pathlib import Path

from test.support.hf import (
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
                    "must specify one of -s, -d, -c, -m, status, stop, or -q",
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
                "name": "duplicate_s",
                "args": ["-s", "out1", "-s", "out2"],
                "rc": 1,
                "stderr_contains": ["duplicate -s", "usage:"],
            },
            {
                "name": "duplicate_d",
                "args": ["-d", "out1", "-d", "out2"],
                "rc": 1,
                "stderr_contains": ["duplicate -d", "usage:"],
            },
            {
                "name": "s_and_d",
                "args": ["-s", "out", "-d", "out2"],
                "rc": 1,
                "stderr_contains": ["cannot use -s -d together", "usage:"],
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
                "args": ["-s", "out", "-m", "hello"],
                "rc": 1,
                "stderr_contains": [
                    "cannot use server and client modes together",
                    "usage:",
                ],
            },
            {
                "name": "control_with_s",
                "args": ["status", "-s", "out"],
                "rc": 1,
                "stderr_contains": ["control mode does not accept -s", "usage:"],
            },
            {
                "name": "control_with_p",
                "args": ["stop", "-p", "9999"],
                "rc": 1,
                "stderr_contains": ["control mode does not accept -p", "usage:"],
            },
            {
                "name": "server_has_i",
                "args": ["-s", "out", "-i", "127.0.0.1"],
                "rc": 1,
                "stderr_contains": ["server mode does not accept -i", "usage:"],
            },
            {
                "name": "control_has_i",
                "args": ["-q", "-i", "127.0.0.1"],
                "rc": 1,
                "stderr_contains": ["control mode does not accept -i", "usage:"],
            },
            {
                "name": "invalid_port",
                "args": ["-d", "out", "-p", "nope"],
                "rc": 1,
                "stderr_contains": ["invalid port", "usage:"],
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

    def test_qr_requires_running_daemon(self) -> None:
        run_hf(self.hf_path, ["stop"], timeout=5.0)
        r = run_hf(self.hf_path, ["-q"], timeout=5.0)
        self.assertEqual(
            1, r.returncode, f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}"
        )
        self.assertIn("no running daemon found", r.stderr)

    @unittest.skipIf(os.name == "nt", "daemon mode is POSIX-only for now")
    def test_daemon_lifecycle_commands(self) -> None:
        run_hf(self.hf_path, ["stop"], timeout=5.0)

        with make_temp_dir(prefix="hf_cli_daemon_") as tmp_dir:
            base_dir = Path(tmp_dir)
            out_dir = base_dir / "outputs"
            out_dir.mkdir(parents=True, exist_ok=True)
            port = reserve_free_port()

            start = run_hf(self.hf_path, ["-d", out_dir, "-p", str(port)], timeout=5.0)
            self.assertEqual(
                0,
                start.returncode,
                f"argv={start.argv} stdout={start.stdout!r} stderr={start.stderr!r}",
            )
            self.assertIn("HFile daemon ready", start.stdout)
            self.assertIn(f":{port}", start.stdout)

            with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/", timeout=5.0
            ) as resp:
                body = resp.read().decode("utf-8", errors="replace")
            self.assertEqual(200, resp.status)
            self.assertIn("HFile", body)

            status = run_hf(self.hf_path, ["status"], timeout=5.0)
            self.assertEqual(
                0,
                status.returncode,
                f"argv={status.argv} stdout={status.stdout!r} stderr={status.stderr!r}",
            )
            self.assertIn("status: running", status.stdout)
            self.assertIn(f"port: {port}", status.stdout)
            self.assertIn(f"receive dir: {out_dir}", status.stdout)
            error_log_line = next(
                line
                for line in status.stdout.splitlines()
                if line.startswith("error log: ")
            )
            error_log_path = Path(error_log_line.split(": ", 1)[1])

            qr = run_hf(self.hf_path, ["-q"], timeout=5.0)
            self.assertEqual(
                0,
                qr.returncode,
                f"argv={qr.argv} stdout={qr.stdout!r} stderr={qr.stderr!r}",
            )
            self.assertIn(f":{port}/", qr.stdout)
            self.assertTrue(any(ch in qr.stdout for ch in ("█", "▀", "▄")))

            src = base_dir / "hello.txt"
            src.write_bytes(b"hello daemon\n")
            send = run_hf(
                self.hf_path,
                ["-c", src, "-i", "127.0.0.1", "-p", str(port)],
                timeout=5.0,
            )
            self.assertEqual(
                0, send.returncode, f"argv={send.argv} stderr={send.stderr!r}"
            )
            self.assertEqual(b"hello daemon\n", (out_dir / src.name).read_bytes())
            self.assertNotIn(
                "saved to", error_log_path.read_text(encoding="utf-8", errors="replace")
            )

            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5.0)
            conn.request("GET", "/api/messages/stream")
            resp = conn.getresponse()
            self.assertEqual(200, resp.status)

            second = run_hf(self.hf_path, ["-d", out_dir, "-p", str(port)], timeout=5.0)
            self.assertEqual(1, second.returncode)
            self.assertIn("HFile is already running", second.stderr)

            stop = run_hf(self.hf_path, ["stop"], timeout=5.0)
            self.assertEqual(
                0,
                stop.returncode,
                f"argv={stop.argv} stdout={stop.stdout!r} stderr={stop.stderr!r}",
            )
            self.assertIn("stopped pid", stop.stdout)
            conn.close()

            stopped = run_hf(self.hf_path, ["status"], timeout=5.0)
            self.assertEqual(1, stopped.returncode)
            self.assertIn("status: stopped", stopped.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
