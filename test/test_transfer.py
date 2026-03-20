from __future__ import annotations

import shutil
import socket
import struct
import re
import unittest
from pathlib import Path

from test.support.hf import (
    HFileServer,
    assert_files_equal,
    make_temp_dir,
    protocol_define,
    resolve_hf_path,
    run_hf,
    tail_text_file,
    wait_for_file_stable,
    wait_for_text_in_file,
)


CHUNK_SIZE = 1024 * 1024
PROTOCOL_MAGIC = protocol_define("HF_PROTOCOL_MAGIC")
PROTOCOL_VERSION = protocol_define("HF_PROTOCOL_VERSION")
MSG_TYPE_FILE_TRANSFER = protocol_define("HF_MSG_TYPE_FILE_TRANSFER")
MSG_TYPE_TEXT_MESSAGE = protocol_define("HF_MSG_TYPE_TEXT_MESSAGE")
MSG_FLAG_NONE = protocol_define("HF_MSG_FLAG_NONE")
MSG_FLAG_COMPRESS = protocol_define("HF_MSG_FLAG_COMPRESS")
COMPRESS_BLOCK_TYPE_RAW = protocol_define("HF_COMPRESS_BLOCK_TYPE_RAW")
MAX_TEXT_MESSAGE_SIZE = protocol_define("HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE")
FIXTURES_DIR = Path(__file__).resolve().parent / "fixtures" / "transfer"


class TransferTestCase(unittest.TestCase):
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

    def _server_log_offset(self) -> int:
        if self.server.log_path is None:
            return 0
        try:
            return int(self.server.log_path.stat().st_size)
        except FileNotFoundError:
            return 0

    def _server_log_tail(self) -> str:
        return tail_text_file(self.server.log_path or Path(""))

    def _wait_for_server_log(
        self, needle: str, *, offset: int = 0, timeout: float = 5.0
    ) -> str:
        log_path = self.server.log_path or Path("")
        log_text = wait_for_text_in_file(
            log_path, needle, offset=offset, timeout=timeout
        )
        if log_text is None:
            self.fail(f"missing {needle!r} in server log: {self._server_log_tail()!r}")
        return log_text

    def _reset_output_path(self, path: Path) -> None:
        if path.exists():
            if path.is_file() or path.is_symlink():
                path.unlink()
            else:
                shutil.rmtree(path, ignore_errors=True)

    def _run_client_file_transfer(
        self,
        src: Path,
        *,
        extra_args: tuple[str, ...] = (),
        timeout: float = 8.0,
    ):
        dst = self.out_dir / src.name
        self._reset_output_path(dst)
        r = run_hf(
            self.hf_path,
            [
                "-c",
                src,
                "-i",
                self.server.host,
                "-p",
                str(self.server.port),
                *extra_args,
            ],
            timeout=timeout,
        )
        return r, dst

    def _send_and_assert_ok(
        self,
        src: Path,
        *,
        extra_args: tuple[str, ...] = (),
        timeout: float = 8.0,
    ) -> Path:
        r, dst = self._run_client_file_transfer(
            src, extra_args=extra_args, timeout=timeout
        )
        self.assertEqual(
            r.returncode,
            0,
            f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertTrue(
            wait_for_file_stable(dst, timeout=max(5.0, timeout)),
            f"file not saved: {dst}; client_stderr={r.stderr!r}; server_log_tail={self._server_log_tail()!r}",
        )
        return dst

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

    def _make_header(
        self,
        *,
        msg_type: int,
        payload_size: int,
        magic: int = PROTOCOL_MAGIC,
        version: int = PROTOCOL_VERSION,
        flags: int = MSG_FLAG_NONE,
    ) -> bytes:
        return struct.pack("!HBBBQ", magic, version, msg_type, flags, payload_size)

    def _make_file_prefix(self, file_name: bytes, content_size: int) -> bytes:
        return (
            struct.pack("!H", len(file_name))
            + file_name
            + struct.pack("!Q", content_size)
        )

    def _make_compressed_raw_block(self, data: bytes) -> bytes:
        return struct.pack("!BII", COMPRESS_BLOCK_TYPE_RAW, len(data), len(data)) + data

    def _client_wire_bytes(self, stderr: str) -> int:
        m = re.search(r"wire_bytes=(\d+)B", stderr)
        self.assertIsNotNone(m, f"missing wire_bytes in stderr={stderr!r}")
        assert m is not None
        return int(m.group(1))

    def _sendall_or_fail(self, sock: socket.socket, data: bytes, *, phase: str) -> None:
        try:
            sock.sendall(data)
        except OSError as e:
            self.fail(
                f"failed to send {phase}: {e}; server_log_tail={self._server_log_tail()!r}"
            )

    def _shutdown_write_or_fail(self, sock: socket.socket, *, phase: str) -> None:
        try:
            sock.shutdown(socket.SHUT_WR)
        except OSError as e:
            self.fail(
                f"failed to shutdown write during {phase}: {e}; server_log_tail={self._server_log_tail()!r}"
            )

    def _recv_ack_or_fail(self, sock: socket.socket, *, phase: str) -> bytes:
        try:
            ack = sock.recv(1)
        except OSError as e:
            self.fail(
                f"failed to receive {phase}: {e}; server_log_tail={self._server_log_tail()!r}"
            )

        if len(ack) != 1:
            self.fail(
                f"missing {phase}: ack={ack!r}; server_log_tail={self._server_log_tail()!r}"
            )
        return ack

    def _send_raw_parts(self, parts: list[bytes], *, timeout: float = 8.0) -> bytes:
        with socket.create_connection(
            (self.server.host, self.server.port), timeout=timeout
        ) as s:
            s.settimeout(timeout)
            for idx, part in enumerate(parts):
                self._sendall_or_fail(s, part, phase=f"raw part {idx}")
            self._shutdown_write_or_fail(s, phase="raw parts")
            return self._recv_ack_or_fail(s, phase="raw parts final ack")

    def _send_raw_file_preamble_and_recv_ack(
        self,
        header: bytes,
        prefix: bytes,
        *,
        timeout: float = 8.0,
    ) -> bytes:
        with socket.create_connection(
            (self.server.host, self.server.port), timeout=timeout
        ) as s:
            s.settimeout(timeout)
            self._sendall_or_fail(s, header, phase="file header")
            self._sendall_or_fail(s, prefix, phase="file prefix")
            return self._recv_ack_or_fail(s, phase="file preamble ack")

    def _send_raw_file_transfer(
        self,
        header: bytes,
        prefix: bytes,
        body: bytes,
        *,
        timeout: float = 8.0,
    ) -> tuple[bytes, bytes]:
        with socket.create_connection(
            (self.server.host, self.server.port), timeout=timeout
        ) as s:
            s.settimeout(timeout)
            self._sendall_or_fail(s, header, phase="file header")
            self._sendall_or_fail(s, prefix, phase="file prefix")
            ready_ack = self._recv_ack_or_fail(s, phase="file ready ack")
            if ready_ack != b"\x00":
                return ready_ack, b""
            if body:
                self._sendall_or_fail(s, body, phase="file body")
            self._shutdown_write_or_fail(s, phase="file body")
            final_ack = self._recv_ack_or_fail(s, phase="file final ack")
            return ready_ack, final_ack


class TestTransferCLI(TransferTestCase):
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
            ("utf8", "你好 hfile", "msg: 你好 hfile"),
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

    def test_file_transfer_compresses_compressible_file(self) -> None:
        data = (b"compressible-data-" * (CHUNK_SIZE // 64)) + b"tail\n"
        src = self._write_input_file("compress.txt", data)
        r, dst = self._run_client_file_transfer(
            src,
            extra_args=("--compress", "--perf"),
            timeout=15.0,
        )
        self.assertEqual(
            r.returncode,
            0,
            f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("perf mode=client ok=1", r.stderr)
        self.assertTrue(
            wait_for_file_stable(dst, timeout=15.0),
            f"file not saved: {dst}; client_stderr={r.stderr!r}; server_log_tail={self._server_log_tail()!r}",
        )
        assert_files_equal(self, src, dst)

        plain_wire_bytes = (
            protocol_define("HF_PROTOCOL_HEADER_SIZE")
            + len(self._make_file_prefix(src.name.encode("utf-8"), src.stat().st_size))
            + src.stat().st_size
        )
        self.assertLess(
            self._client_wire_bytes(r.stderr),
            plain_wire_bytes,
            f"expected compressed transfer to shrink wire bytes; stderr={r.stderr!r}",
        )

    def test_file_transfer_compresses_multi_chunk_file(self) -> None:
        size = (CHUNK_SIZE * 2) + 137
        data = (b"multi-chunk-compress-" * ((size // 21) + 1))[:size]
        src = self._write_input_file("compress_multi_chunk.txt", data)
        dst = self._send_and_assert_ok(
            src,
            extra_args=("--compress",),
            timeout=20.0,
        )
        assert_files_equal(self, src, dst)

    def test_server_rejects_invalid_file_name_from_client(self) -> None:
        src = self._write_input_file("bad..name.txt", b"bad name\n")
        dst = self.out_dir / src.name
        self._reset_output_path(dst)

        log_offset = self._server_log_offset()
        r, _ = self._run_client_file_transfer(src)
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
                    f"perf transfer did not save {dst}; server_log_tail={tail_text_file(server.log_path or Path(''))!r}",
                )

                log_text = wait_for_text_in_file(
                    server.log_path or Path(""),
                    "perf mode=server ok=1",
                    timeout=5.0,
                )
                if log_text is None:
                    self.fail(
                        f"missing 'perf mode=server ok=1' in server log: {tail_text_file(server.log_path or Path(''))!r}"
                    )
            finally:
                server.stop()

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


class TestTransferProtocol(TransferTestCase):
    def test_protocol_rejects_invalid_magic(self) -> None:
        header = self._make_header(
            msg_type=MSG_TYPE_TEXT_MESSAGE,
            payload_size=0,
            magic=0x1234,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header])
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected invalid-magic ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: failed to decode header", offset=log_offset
        )

    def test_protocol_rejects_invalid_version(self) -> None:
        header = self._make_header(
            msg_type=MSG_TYPE_TEXT_MESSAGE,
            payload_size=0,
            version=PROTOCOL_VERSION + 1,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header])
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected invalid-version ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: failed to decode header", offset=log_offset
        )

    def test_text_protocol_rejects_unsupported_flags(self) -> None:
        header = self._make_header(
            msg_type=MSG_TYPE_TEXT_MESSAGE,
            payload_size=0,
            flags=MSG_FLAG_COMPRESS,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header])
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected text-unsupported-flags ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: text message has unsupported compress flag", offset=log_offset
        )

    def test_file_protocol_rejects_unsupported_flags(self) -> None:
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=0,
            flags=0x02,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header])
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected file-unsupported-flags ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: failed to decode header", offset=log_offset
        )

    def test_file_protocol_rejects_payload_size_too_small(self) -> None:
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=9,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header])
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected payload-too-small ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: payload size too small", offset=log_offset
        )

    def test_protocol_rejects_unsupported_message_type(self) -> None:
        header = self._make_header(
            msg_type=0x7F,
            payload_size=0,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header])
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected unsupported-message ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: failed to decode header",
            offset=log_offset,
        )
        self.assertFalse(list(self.out_dir.glob("unsupported.bin*")))

    def test_text_message_too_large(self) -> None:
        header = self._make_header(
            msg_type=MSG_TYPE_TEXT_MESSAGE,
            payload_size=MAX_TEXT_MESSAGE_SIZE + 1,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header])
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected text-too-large ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )

    def test_protocol_rejects_invalid_file_name_before_body(self) -> None:
        file_name = b"bad..name.bin"
        content_size = 1024
        prefix = self._make_file_prefix(file_name, content_size)
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=len(prefix) + content_size,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_file_preamble_and_recv_ack(header, prefix)
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected invalid-name pre-body ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            f"invalid file name: {file_name.decode('ascii')}", offset=log_offset
        )
        self.assertFalse((self.out_dir / file_name.decode("ascii")).exists())
        self._assert_no_temp_files(file_name.decode("ascii"))

    def test_protocol_rejects_payload_size_mismatch(self) -> None:
        file_name = b"mismatch.bin"
        content_size = 2
        prefix = self._make_file_prefix(file_name, content_size)
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=len(prefix) + content_size + 1,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_file_preamble_and_recv_ack(header, prefix)
        self.assertEqual(
            ack,
            b"\x01",
            f"unexpected payload-mismatch pre-body ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: payload size mismatch", offset=log_offset
        )
        self.assertFalse((self.out_dir / file_name.decode("ascii")).exists())

    def test_raw_file_transfer_two_phase_success(self) -> None:
        file_name = b"raw_ok.bin"
        body = b"raw success\n"
        prefix = self._make_file_prefix(file_name, len(body))
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=len(prefix) + len(body),
        )

        dst = self.out_dir / file_name.decode("ascii")
        self._reset_output_path(dst)

        with socket.create_connection(
            (self.server.host, self.server.port), timeout=8.0
        ) as s:
            s.settimeout(8.0)
            self._sendall_or_fail(s, header, phase="file header")
            self._sendall_or_fail(s, prefix, phase="file prefix")
            ready_ack = self._recv_ack_or_fail(s, phase="file ready ack")
            self.assertEqual(
                ready_ack,
                b"\x00",
                f"unexpected ready ack: {ready_ack!r}; server_log_tail={self._server_log_tail()!r}",
            )
            self.assertFalse(
                dst.exists(), f"final file appeared before body send: {dst}"
            )
            self._sendall_or_fail(s, body, phase="file body")
            self._shutdown_write_or_fail(s, phase="file body")
            final_ack = self._recv_ack_or_fail(s, phase="file final ack")

        self.assertEqual(
            final_ack,
            b"\x00",
            f"unexpected final ack: {final_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertTrue(
            wait_for_file_stable(dst, timeout=8.0),
            f"raw transfer did not save {dst}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertEqual(
            body, dst.read_bytes(), f"raw transfer content mismatch for {dst}"
        )
        self._assert_no_temp_files(file_name.decode("ascii"))

    def test_compressed_file_protocol_accepts_raw_blocks(self) -> None:
        file_name = b"raw_compress.bin"
        parts = [b"compress raw ", b"path\n"]
        body = b"".join(self._make_compressed_raw_block(part) for part in parts)
        prefix = self._make_file_prefix(file_name, sum(len(part) for part in parts))
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=len(prefix) + len(body),
            flags=MSG_FLAG_COMPRESS,
        )

        dst = self.out_dir / file_name.decode("ascii")
        self._reset_output_path(dst)
        ready_ack, final_ack = self._send_raw_file_transfer(header, prefix, body)
        self.assertEqual(
            ready_ack,
            b"\x00",
            f"unexpected compress ready ack: {ready_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertEqual(
            final_ack,
            b"\x00",
            f"unexpected compress final ack: {final_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertTrue(
            wait_for_file_stable(dst, timeout=8.0),
            f"compressed raw transfer did not save {dst}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertEqual(
            b"".join(parts),
            dst.read_bytes(),
            f"raw compress content mismatch for {dst}",
        )

    def test_compressed_file_protocol_rejects_invalid_block_type(self) -> None:
        file_name = b"bad_block_type.bin"
        content = b"abc"
        body = struct.pack("!BII", 0x7F, len(content), len(content)) + content
        prefix = self._make_file_prefix(file_name, len(content))
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=len(prefix) + len(body),
            flags=MSG_FLAG_COMPRESS,
        )

        log_offset = self._server_log_offset()
        ready_ack, final_ack = self._send_raw_file_transfer(header, prefix, body)
        self.assertEqual(
            ready_ack,
            b"\x00",
            f"unexpected ready ack: {ready_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        # self.assertEqual(
        #     final_ack,
        #     b"\x01",
        #     f"unexpected final ack: {final_ack!r}; server_log_tail={self._server_log_tail()!r}",
        # )
        self._wait_for_server_log(
            "protocol error: invalid compressed block type", offset=log_offset
        )
        self.assertFalse((self.out_dir / file_name.decode("ascii")).exists())

    def test_compressed_file_protocol_rejects_raw_block_size_mismatch(self) -> None:
        file_name = b"bad_raw_block.bin"
        content = b"abc"
        body = (
            struct.pack("!BII", COMPRESS_BLOCK_TYPE_RAW, len(content), len(content) - 1)
            + content[:2]
        )
        prefix = self._make_file_prefix(file_name, len(content))
        header = self._make_header(
            msg_type=MSG_TYPE_FILE_TRANSFER,
            payload_size=len(prefix) + len(body),
            flags=MSG_FLAG_COMPRESS,
        )

        log_offset = self._server_log_offset()
        ready_ack, final_ack = self._send_raw_file_transfer(header, prefix, body)
        self.assertEqual(
            ready_ack,
            b"\x00",
            f"unexpected ready ack: {ready_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertEqual(
            final_ack,
            b"\x01",
            f"unexpected final ack: {final_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: raw compressed block size mismatch", offset=log_offset
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
        ready_ack, final_ack = self._send_raw_file_transfer(header, prefix, b"x" * 100)
        self.assertEqual(
            ready_ack,
            b"\x00",
            f"unexpected ready ack: {ready_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertEqual(
            final_ack,
            b"\x01",
            f"unexpected final ack: {final_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: unexpected EOF while receiving file",
            offset=log_offset,
        )

        final_name = file_name.decode("ascii")
        self.assertFalse((self.out_dir / final_name).exists())
        self._assert_no_temp_files(final_name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
