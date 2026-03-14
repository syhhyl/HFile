from __future__ import annotations

import shutil
import socket
import struct
import time
import unittest
from pathlib import Path

from test.support.hf import (
    HFileServer,
    assert_files_equal,
    make_temp_dir,
    resolve_hf_path,
    run_hf,
    tail_text_file,
    wait_for_file_stable,
)


CHUNK_SIZE = 1024 * 1024
PROTOCOL_MAGIC = 0x0429
PROTOCOL_VERSION = 0x02
MSG_TYPE_FILE_TRANSFER = 0x01
MSG_TYPE_TEXT_MESSAGE = 0x02
FIXTURES_DIR = Path(__file__).resolve().parent / "fixtures" / "transfer"


class TestTransfer(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            cls.hf_path = resolve_hf_path()
        except FileNotFoundError as e:
            raise unittest.SkipTest(str(e))

        cls._tmp = make_temp_dir(prefix="hf_transfer_")
        cls.base_dir = Path(cls._tmp.name)
        cls.in_dir = cls.base_dir / "inputs"
        cls.out_dir = cls.base_dir / "outputs"
        cls.in_dir.mkdir(parents=True, exist_ok=True)
        cls.out_dir.mkdir(parents=True, exist_ok=True)

        cls.server = HFileServer(hf_path=cls.hf_path, out_dir=cls.out_dir)
        cls.server.start(startup_timeout=5.0)

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "server"):
            cls.server.stop()
        if hasattr(cls, "_tmp"):
            cls._tmp.cleanup()

    def _send_and_assert_ok(self, src: Path, *, timeout: float = 8.0) -> Path:
        dst = self.out_dir / src.name
        if dst.exists():
            if dst.is_file() or dst.is_symlink():
                dst.unlink()
            else:
                shutil.rmtree(dst, ignore_errors=True)

        r = run_hf(
            self.hf_path,
            ["-c", src, "-i", self.server.host, "-p", str(self.server.port)],
            timeout=timeout,
        )
        self.assertEqual(
            r.returncode,
            0,
            f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )

        self.assertTrue(
            wait_for_file_stable(dst, timeout=max(5.0, timeout)),
            (
                f"file not saved: {dst}; client_stderr={r.stderr!r}; "
                f"server_log_tail={tail_text_file(self.server.log_path or Path(''))!r}"
            ),
        )
        return dst

    def _server_log_offset(self) -> int:
        if self.server.log_path is None:
            return 0
        try:
            return int(self.server.log_path.stat().st_size)
        except FileNotFoundError:
            return 0

    def _server_log_text(self, *, offset: int = 0) -> str:
        if self.server.log_path is None:
            return ""
        try:
            data = self.server.log_path.read_bytes()
        except FileNotFoundError:
            return ""
        return data[offset:].decode("utf-8", errors="replace")

    def _wait_for_server_log(
        self, needle: str, *, offset: int = 0, timeout: float = 5.0
    ) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            log_text = self._server_log_text(offset=offset)
            if needle in log_text:
                return log_text
            time.sleep(0.05)

        self.fail(
            f"missing {needle!r} in server log: "
            f"{tail_text_file(self.server.log_path or Path(''))!r}"
        )

    def _make_header(
        self,
        *,
        msg_type: int,
        payload_size: int,
        magic: int = PROTOCOL_MAGIC,
        version: int = PROTOCOL_VERSION,
        flags: int = 0,
    ) -> bytes:
        return struct.pack("!HBBBQ", magic, version, msg_type, flags, payload_size)

    def _make_file_prefix(self, file_name: bytes, content_size: int) -> bytes:
        return (
            struct.pack("!H", len(file_name))
            + file_name
            + struct.pack("!Q", content_size)
        )

    def _send_raw_parts(self, parts: list[bytes], *, timeout: float = 8.0) -> bytes:
        with socket.create_connection(
            (self.server.host, self.server.port), timeout=timeout
        ) as s:
            s.settimeout(timeout)
            for part in parts:
                s.sendall(part)
            s.shutdown(socket.SHUT_WR)
            return s.recv(1)

    def _assert_no_temp_files(self, final_name: str) -> None:
        matches = sorted(
            p.name
            for p in self.out_dir.iterdir()
            if p.name.startswith(f"{final_name}.tmp.")
        )
        self.assertFalse(matches, f"unexpected temp files left behind: {matches}")

    def _write_input_file(self, name: str, data: bytes) -> Path:
        src = self.in_dir / name
        src.write_bytes(data)
        return src

    def _copy_fixture_to_input(self, fixture: Path) -> Path:
        src = self.in_dir / fixture.name
        shutil.copy2(fixture, src)
        return src

    def test_common_file(self) -> None:
        src = self._write_input_file("hello.txt", b"hello hfile\n")
        dst = self._send_and_assert_ok(src)
        assert_files_equal(self, src, dst)

    def test_empty_file(self) -> None:
        src = self._write_input_file("empty.txt", b"")
        dst = self._send_and_assert_ok(src)
        assert_files_equal(self, src, dst)

    def test_text_messages(self) -> None:
        cases = [
            ("ascii", "hello hfile", "msg: hello hfile"),
            ("empty", "", "msg: "),
            ("utf8", "\u4f60\u597d hfile", "msg: \u4f60\u597d hfile"),
        ]

        for name, message, needle in cases:
            with self.subTest(name=name):
                log_offset = self._server_log_offset()
                r = run_hf(
                    self.hf_path,
                    [
                        "-m",
                        message,
                        "-i",
                        self.server.host,
                        "-p",
                        str(self.server.port),
                    ],
                    timeout=8.0,
                )
                self.assertEqual(
                    r.returncode,
                    0,
                    f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
                )
                self._wait_for_server_log(needle, offset=log_offset)

    def test_file_chunk_boundaries(self) -> None:
        sizes = [CHUNK_SIZE - 1, CHUNK_SIZE, CHUNK_SIZE + 1]

        for size in sizes:
            with self.subTest(size=size):
                pattern = (b"0123456789abcdef" * ((size + 15) // 16))[:size]
                src = self._write_input_file(f"chunk_{size}.bin", pattern)
                dst = self._send_and_assert_ok(src, timeout=15.0)
                assert_files_equal(self, src, dst)

    def test_existing_file_is_overwritten(self) -> None:
        src = self._write_input_file("overwrite.txt", b"first version\n")
        dst = self._send_and_assert_ok(src)
        assert_files_equal(self, src, dst)

        src.write_bytes(b"second version is longer\n")
        dst = self._send_and_assert_ok(src)
        assert_files_equal(self, src, dst)

    def test_file_transfer_accepts_compress_flag(self) -> None:
        src = self._write_input_file(
            "compress.txt", b"compress flag is currently a no-op\n"
        )
        dst = self.out_dir / src.name
        if dst.exists():
            dst.unlink()

        r = run_hf(
            self.hf_path,
            [
                "-c",
                src,
                "-i",
                self.server.host,
                "-p",
                str(self.server.port),
                "--compress",
            ],
            timeout=8.0,
        )
        self.assertEqual(
            r.returncode,
            0,
            f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertTrue(
            wait_for_file_stable(dst, timeout=8.0),
            f"compressed transfer did not save {dst}",
        )
        assert_files_equal(self, src, dst)

    def test_server_rejects_invalid_file_name_from_client(self) -> None:
        src = self._write_input_file("bad..name.txt", b"bad name\n")
        dst = self.out_dir / src.name
        if dst.exists():
            dst.unlink()

        log_offset = self._server_log_offset()
        r = run_hf(
            self.hf_path,
            ["-c", src, "-i", self.server.host, "-p", str(self.server.port)],
            timeout=8.0,
        )
        self.assertEqual(
            r.returncode,
            1,
            f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("server reported transfer failure", r.stderr)
        self._wait_for_server_log(f"invalid file name: {src.name}", offset=log_offset)
        self.assertFalse(dst.exists(), f"unexpected output file: {dst}")
        self._assert_no_temp_files(src.name)

    def test_perf_option_reports_on_client_and_server(self) -> None:
        with make_temp_dir(prefix="hf_perf_") as tmp_dir:
            base_dir = Path(tmp_dir)
            out_dir = base_dir / "out"
            out_dir.mkdir(parents=True, exist_ok=True)

            server = HFileServer(
                hf_path=self.hf_path,
                out_dir=out_dir,
                extra_args=["--perf"],
            )
            server.start(startup_timeout=5.0)
            try:
                src = self._write_input_file("perf.txt", b"perf\n")
                dst = out_dir / src.name
                r = run_hf(
                    self.hf_path,
                    [
                        "-c",
                        src,
                        "-i",
                        server.host,
                        "-p",
                        str(server.port),
                        "--perf",
                    ],
                    timeout=8.0,
                )
                self.assertEqual(
                    r.returncode,
                    0,
                    f"argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
                )
                self.assertIn("perf mode=client ok=1", r.stderr)
                self.assertTrue(
                    wait_for_file_stable(dst, timeout=8.0),
                    f"perf transfer did not save {dst}",
                )

                deadline = time.monotonic() + 5.0
                needle = "perf mode=server ok=1"
                while time.monotonic() < deadline:
                    log_text = tail_text_file(server.log_path or Path(""))
                    if needle in log_text:
                        break
                    time.sleep(0.05)
                else:
                    self.fail(
                        f"missing {needle!r} in server log: "
                        f"{tail_text_file(server.log_path or Path(''))!r}"
                    )
            finally:
                server.stop()

    def test_text_message_too_large(self) -> None:
        payload_size = 256 * 1024 + 1
        header = struct.pack("!HBBBQ", 0x0429, 0x02, 0x02, 0x00, payload_size)

        with socket.create_connection(
            (self.server.host, self.server.port), timeout=8.0
        ) as s:
            s.sendall(header)
            s.shutdown(socket.SHUT_WR)
            ack = s.recv(1)

        self.assertEqual(ack, b"\x01", f"unexpected ack: {ack!r}")

        deadline = time.monotonic() + 5.0
        needle = "protocol error: message payload too large"
        while time.monotonic() < deadline:
            log_text = tail_text_file(self.server.log_path or Path(""))
            if needle in log_text:
                break
            time.sleep(0.05)
        else:
            self.fail(
                f"missing {needle!r} in server log: "
                f"{tail_text_file(self.server.log_path or Path(''))!r}"
            )

    def test_protocol_header_validation(self) -> None:
        cases = [
            {
                "name": "invalid_magic",
                "header": self._make_header(
                    msg_type=MSG_TYPE_TEXT_MESSAGE,
                    payload_size=0,
                    magic=0x1234,
                ),
                "needle": "protocol error: invalid protocol magic",
            },
            {
                "name": "invalid_version",
                "header": self._make_header(
                    msg_type=MSG_TYPE_TEXT_MESSAGE,
                    payload_size=0,
                    version=0x03,
                ),
                "needle": "protocol error: unsupported protocol version",
            },
            {
                "name": "unsupported_flags",
                "header": self._make_header(
                    msg_type=MSG_TYPE_TEXT_MESSAGE,
                    payload_size=0,
                    flags=0x01,
                ),
                "needle": "protocol error: unsupported flags: 1",
            },
            {
                "name": "payload_too_small",
                "header": self._make_header(
                    msg_type=MSG_TYPE_FILE_TRANSFER,
                    payload_size=9,
                ),
                "needle": "protocol error: payload size too small",
            },
        ]

        for c in cases:
            with self.subTest(name=c["name"]):
                log_offset = self._server_log_offset()
                ack = self._send_raw_parts([c["header"]])
                self.assertEqual(ack, b"\x01", f"unexpected ack: {ack!r}")
                self._wait_for_server_log(c["needle"], offset=log_offset)

    def test_protocol_rejects_unsupported_message_type(self) -> None:
        file_name = b"unsupported.bin"
        content = b"blocked\n"
        prefix = self._make_file_prefix(file_name, len(content))
        header = self._make_header(
            msg_type=0x7F,
            payload_size=len(prefix) + len(content),
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header, prefix, content])
        self.assertEqual(ack, b"\x01", f"unexpected ack: {ack!r}")
        self._wait_for_server_log(
            "protocol error: unsupported message type: 127",
            offset=log_offset,
        )
        final_name = file_name.decode("ascii")
        self.assertFalse((self.out_dir / final_name).exists())
        self._assert_no_temp_files(final_name)

    def test_protocol_rejects_payload_size_mismatch(self) -> None:
        file_name = b"mismatch.bin"
        content_size = 2
        prefix = self._make_file_prefix(file_name, content_size)
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=len(prefix) + content_size + 1,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header, prefix])
        self.assertEqual(ack, b"\x01", f"unexpected ack: {ack!r}")
        self._wait_for_server_log(
            "protocol error: payload size mismatch", offset=log_offset
        )
        self.assertFalse((self.out_dir / file_name.decode("ascii")).exists())

    def test_partial_raw_file_transfer_cleans_up_temp_file(self) -> None:
        file_name = b"partial.bin"
        content_size = 1024
        prefix = self._make_file_prefix(file_name, content_size)
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=len(prefix) + content_size,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header, prefix, b"x" * 100])
        self.assertEqual(ack, b"\x01", f"unexpected ack: {ack!r}")
        self._wait_for_server_log(
            "protocol error: unexpected EOF while receiving file",
            offset=log_offset,
        )

        final_name = file_name.decode("ascii")
        self.assertFalse((self.out_dir / final_name).exists())
        self._assert_no_temp_files(final_name)

    def test_fixtures(self) -> None:
        if not FIXTURES_DIR.exists():
            self.skipTest(f"fixtures dir not found: {FIXTURES_DIR}")

        fixtures = sorted(
            [
                p
                for p in FIXTURES_DIR.iterdir()
                if p.is_file() and not p.name.startswith(".")
            ],
            key=lambda p: p.name,
        )
        if not fixtures:
            self.skipTest(f"no fixtures found in: {FIXTURES_DIR}")

        for fixture in fixtures:
            with self.subTest(fixture=fixture.name):
                src = self._copy_fixture_to_input(fixture)
                dst = self._send_and_assert_ok(src)
                assert_files_equal(self, src, dst)


if __name__ == "__main__":
    unittest.main(verbosity=2)
