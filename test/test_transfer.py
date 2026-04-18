from __future__ import annotations

import os
import signal
import shutil
import socket
import struct
import threading
import time
import unittest
from pathlib import Path

from test.support.hf import (
    HFileServer,
    assert_files_equal,
    make_temp_dir,
    protocol_define,
    reserve_free_port,
    resolve_hf_path,
    run_hf,
    tail_text_file,
    wait_for_file_stable,
    wait_for_text_in_file,
)


CHUNK_SIZE = 1024 * 1024
PROTOCOL_MAGIC = protocol_define("HF_PROTOCOL_MAGIC")
PROTOCOL_VERSION = protocol_define("HF_PROTOCOL_VERSION")
MSG_TYPE_SEND_FILE = protocol_define("HF_MSG_TYPE_SEND_FILE")
MSG_TYPE_TEXT_MESSAGE = protocol_define("HF_MSG_TYPE_TEXT_MESSAGE")
MSG_FLAG_NONE = protocol_define("HF_MSG_FLAG_NONE")
MAX_TEXT_MESSAGE_SIZE = protocol_define("HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE")
MSG_TYPE_GET_FILE = protocol_define("HF_MSG_TYPE_GET_FILE")
FIXTURES_DIR = Path(__file__).resolve().parent / "fixtures" / "transfer"


class FakeDownloadServer:
    def __init__(
        self,
        *,
        offered_name: bytes,
        body: bytes,
        final_frame: bytes | None,
        advertised_size: int | None = None,
        host: str = "127.0.0.1",
    ) -> None:
        self.host = host
        self.port = reserve_free_port(host=host)
        self.offered_name = offered_name
        self.body = body
        self.final_frame = final_frame
        self.advertised_size = len(body) if advertised_size is None else advertised_size
        self._listener: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._ready = threading.Event()
        self._error: BaseException | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        if not self._ready.wait(timeout=5.0):
            raise RuntimeError("fake download server did not start")

    def stop(self) -> None:
        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass
        if self._thread is not None:
            self._thread.join(timeout=5.0)
        if self._error is not None:
            raise RuntimeError(f"fake download server failed: {self._error}")

    def _recv_exact(self, conn: socket.socket, size: int) -> bytes:
        chunks: list[bytes] = []
        remaining = size
        while remaining > 0:
            chunk = conn.recv(remaining)
            if not chunk:
                raise RuntimeError("client closed connection early")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _serve(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener = listener
        try:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((self.host, self.port))
            listener.listen(1)
            self._ready.set()
            conn, _ = listener.accept()
            with conn:
                header = self._recv_exact(conn, 13)
                payload_size = struct.unpack("!HBBBQ", header)[4]
                if payload_size > 0:
                    self._recv_exact(conn, payload_size)

                conn.sendall(struct.pack("!BBH", 0, 0, 0))
                conn.sendall(
                    struct.pack("!H", len(self.offered_name))
                    + self.offered_name
                    + struct.pack("!Q", self.advertised_size)
                )
                if self.body:
                    conn.sendall(self.body)
                if self.final_frame is not None:
                    conn.sendall(self.final_frame)
        except BaseException as e:
            self._error = e
            self._ready.set()
        finally:
            try:
                listener.close()
            except OSError:
                pass


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
        cls.download_dir = cls.base_dir / "downloads"
        cls.in_dir.mkdir(parents=True, exist_ok=True)
        cls.out_dir.mkdir(parents=True, exist_ok=True)
        cls.download_dir.mkdir(parents=True, exist_ok=True)

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

    def _make_res_frame(self, phase: int, status: int, error_code: int) -> bytes:
        return struct.pack("!BBH", phase, status, error_code)

    def _recv_res_frame_or_fail(
        self,
        sock: socket.socket,
        *,
        phase: str,
        allow_empty: bool = False,
    ) -> bytes:
        try:
            ack = sock.recv(4)
        except OSError as e:
            self.fail(
                f"failed to receive {phase}: {e}; server_log_tail={self._server_log_tail()!r}"
            )

        if len(ack) == 0 and not allow_empty:
            self.fail(
                f"connection closed before {phase}: ack={ack!r}; server_log_tail={self._server_log_tail()!r}"
            )
        return ack

    def _send_raw_parts(
        self,
        parts: list[bytes],
        *,
        timeout: float = 8.0,
        allow_empty_ack: bool = False,
    ) -> bytes:
        with socket.create_connection(
            (self.server.host, self.server.port), timeout=timeout
        ) as s:
            s.settimeout(timeout)
            for idx, part in enumerate(parts):
                self._sendall_or_fail(s, part, phase=f"raw part {idx}")
            self._shutdown_write_or_fail(s, phase="raw parts")
            return self._recv_res_frame_or_fail(
                s,
                phase="raw parts final ack",
                allow_empty=allow_empty_ack,
            )

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
            return self._recv_res_frame_or_fail(s, phase="file preamble ack")

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
            ready_ack = self._recv_res_frame_or_fail(s, phase="file ready ack")
            if ready_ack[1] != 0:
                return ready_ack, b""
            if body:
                self._sendall_or_fail(s, body, phase="file body")
            self._shutdown_write_or_fail(s, phase="file body")
            final_ack = self._recv_res_frame_or_fail(s, phase="file final ack")
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
            ("ascii", "hello hfile"),
            ("empty", ""),
            ("utf8", "你好 hfile"),
        ]

        for name, message in cases:
            with self.subTest(name=name):
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

    def test_file_chunk_boundaries(self) -> None:
        sizes = [CHUNK_SIZE - 1, CHUNK_SIZE, CHUNK_SIZE + 1]

        for size in sizes:
            with self.subTest(size=size):
                pattern = (b"0123456789abcdef" * ((size + 15) // 16))[:size]
                src = self._write_input_file(f"chunk_{size}.bin", pattern)
                dst = self._send_and_assert_ok(src, timeout=15.0)
                assert_files_equal(self, src, dst)

    def test_raw_file_transfer_large_multi_chunk(self) -> None:
        size = (CHUNK_SIZE * 3) + 517
        data = (b"raw-multi-chunk-" * ((size // 16) + 1))[:size]
        src = self._write_input_file("raw_multi_chunk.bin", data)
        dst = self._send_and_assert_ok(src, timeout=20.0)
        assert_files_equal(self, src, dst)

    def test_existing_file_is_overwritten(self) -> None:
        src = self._write_input_file("overwrite.txt", b"first version\n")
        dst = self._send_and_assert_ok(src)
        assert_files_equal(self, src, dst)

        src.write_bytes(b"second version is longer\n")
        dst = self._send_and_assert_ok(src)
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

    def test_get_downloads_uploaded_file(self) -> None:
        src = self._write_input_file("download.txt", b"download me\n")
        self._send_and_assert_ok(src)

        download_dst = self.download_dir / "download-copy.txt"
        self._reset_output_path(download_dst)

        r = run_hf(
            self.hf_path,
            [
                "-g",
                src.name,
                "-o",
                download_dst,
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
        self.assertTrue(
            wait_for_file_stable(download_dst, timeout=5.0),
            f"download not saved: {download_dst}; stderr={r.stderr!r}",
        )
        assert_files_equal(self, src, download_dst)

    def test_get_rejects_missing_remote_file(self) -> None:
        download_dst = self.download_dir / "missing.txt"
        self._reset_output_path(download_dst)

        r = run_hf(
            self.hf_path,
            [
                "-g",
                "missing.txt",
                "-o",
                download_dst,
                "-i",
                self.server.host,
                "-p",
                str(self.server.port),
            ],
            timeout=8.0,
        )

        self.assertEqual(
            r.returncode,
            1,
            f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("server reported get failure", r.stderr)
        self.assertFalse(download_dst.exists())

    def test_get_rejects_invalid_offered_name(self) -> None:
        server = FakeDownloadServer(
            offered_name=b"../escape.txt",
            body=b"",
            final_frame=None,
        )
        server.start()
        try:
            escaped_path = self.base_dir / "escape.txt"
            self._reset_output_path(escaped_path)

            r = run_hf(
                self.hf_path,
                ["-g", "download.txt", "-i", server.host, "-p", str(server.port)],
                cwd=self.download_dir,
                timeout=8.0,
            )
        finally:
            server.stop()

        self.assertEqual(
            r.returncode,
            1,
            f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("invalid offered file name", r.stderr)
        self.assertFalse(escaped_path.exists())

    def test_get_rejects_oversized_download(self) -> None:
        server = FakeDownloadServer(
            offered_name=b"huge.bin",
            body=b"",
            final_frame=None,
            advertised_size=(100 * 1024 * 1024 * 1024) + 1,
        )
        server.start()
        try:
            download_dst = self.download_dir / "huge.bin"
            self._reset_output_path(download_dst)

            r = run_hf(
                self.hf_path,
                [
                    "-g",
                    "huge.bin",
                    "-o",
                    download_dst,
                    "-i",
                    server.host,
                    "-p",
                    str(server.port),
                ],
                timeout=8.0,
            )
        finally:
            server.stop()

        self.assertEqual(
            r.returncode,
            1,
            f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("MAX_FILE_SIZE is 100GB", r.stderr)
        self.assertFalse(download_dst.exists())
        self.assertFalse(
            list(self.download_dir.glob(f"{download_dst.name}.tmp.*")),
            "unexpected temp download files left behind",
        )

    def test_get_does_not_publish_file_when_final_fails(self) -> None:
        download_dst = self.download_dir / "final-failed.txt"
        self._reset_output_path(download_dst)

        server = FakeDownloadServer(
            offered_name=b"server.txt",
            body=b"body before final failure\n",
            final_frame=self._make_res_frame(1, 2, 9),
        )
        server.start()
        try:
            r = run_hf(
                self.hf_path,
                [
                    "-g",
                    "server.txt",
                    "-o",
                    download_dst,
                    "-i",
                    server.host,
                    "-p",
                    str(server.port),
                ],
                timeout=8.0,
            )
        finally:
            server.stop()

        self.assertEqual(
            r.returncode,
            1,
            f"client failed argv={r.argv} stdout={r.stdout!r} stderr={r.stderr!r}",
        )
        self.assertIn("server reported get failure", r.stderr)
        self.assertFalse(download_dst.exists())
        self.assertFalse(
            list(self.download_dir.glob(f"{download_dst.name}.tmp.*")),
            "unexpected temp download files left behind",
        )

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
        ack = self._send_raw_parts([header], allow_empty_ack=True)
        self.assertEqual(
            ack,
            b"",
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
        ack = self._send_raw_parts([header], allow_empty_ack=True)
        self.assertEqual(
            ack,
            b"",
            f"unexpected invalid-version ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: failed to decode header", offset=log_offset
        )

    def test_file_protocol_rejects_payload_size_too_small(self) -> None:
        header = self._make_header(
            msg_type=MSG_TYPE_SEND_FILE,
            payload_size=9,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header], allow_empty_ack=True)
        self.assertEqual(
            ack,
            self._make_res_frame(0, 1, 5),
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
        ack = self._send_raw_parts([header], allow_empty_ack=True)
        self.assertEqual(
            ack,
            b"",
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
        response = self._send_raw_parts([header])
        self.assertEqual(
            response,
            self._make_res_frame(1, 2, 13),
            f"unexpected text-too-large response: {response!r}; server_log_tail={self._server_log_tail()!r}",
        )

    def test_text_message_success_returns_final_response(self) -> None:
        for name, payload in [("ascii", b"hello raw message"), ("empty", b"")]:
            with self.subTest(name=name):
                header = self._make_header(
                    msg_type=MSG_TYPE_TEXT_MESSAGE,
                    payload_size=len(payload),
                )

                response = self._send_raw_parts([header, payload])
                self.assertEqual(
                    response,
                    self._make_res_frame(1, 0, 0),
                    f"unexpected text-message response: {response!r}; server_log_tail={self._server_log_tail()!r}",
                )

    def test_protocol_rejects_invalid_file_name_before_body(self) -> None:
        file_name = b"bad..name.bin"
        content_size = 1024
        prefix = self._make_file_prefix(file_name, content_size)
        header = self._make_header(
            msg_type=MSG_TYPE_SEND_FILE,
            payload_size=len(prefix) + content_size,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_file_preamble_and_recv_ack(header, prefix)
        self.assertEqual(
            ack,
            self._make_res_frame(0, 1, 7),
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
            msg_type=MSG_TYPE_SEND_FILE,
            payload_size=len(prefix) + content_size + 1,
        )

        log_offset = self._server_log_offset()
        ack = self._send_raw_file_preamble_and_recv_ack(header, prefix)
        self.assertEqual(
            ack,
            self._make_res_frame(0, 1, 8),
            f"unexpected payload-mismatch pre-body ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: payload size mismatch", offset=log_offset
        )
        self.assertFalse((self.out_dir / file_name.decode("ascii")).exists())

    def test_get_protocol_rejects_truncated_request(self) -> None:
        header = self._make_header(
            msg_type=MSG_TYPE_GET_FILE,
            payload_size=2 + len(b"hello.txt"),
        )
        truncated_payload = struct.pack("!H", len(b"hello.txt")) + b"hel"

        log_offset = self._server_log_offset()
        ack = self._send_raw_parts([header, truncated_payload], allow_empty_ack=True)
        self.assertEqual(
            ack,
            self._make_res_frame(0, 1, 12),
            f"unexpected truncated-get ack: {ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: unexpected EOF while receiving get request",
            offset=log_offset,
        )

    def test_raw_file_transfer_two_phase_success(self) -> None:
        file_name = b"raw_ok.bin"
        body = b"raw success\n"
        prefix = self._make_file_prefix(file_name, len(body))
        header = self._make_header(
            msg_type=MSG_TYPE_SEND_FILE,
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
            ready_ack = self._recv_res_frame_or_fail(s, phase="file ready ack")
            self.assertEqual(
                ready_ack[1],
                0,
                f"unexpected ready ack: {ready_ack!r}; server_log_tail={self._server_log_tail()!r}",
            )
            self.assertFalse(
                dst.exists(), f"final file appeared before body send: {dst}"
            )
            self._sendall_or_fail(s, body, phase="file body")
            self._shutdown_write_or_fail(s, phase="file body")
            final_ack = self._recv_res_frame_or_fail(s, phase="file final ack")

        self.assertEqual(
            final_ack[1],
            0,
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

    def test_raw_file_transfer_two_phase_empty_body_success(self) -> None:
        file_name = b"raw_empty.bin"
        body = b""
        prefix = self._make_file_prefix(file_name, len(body))
        header = self._make_header(
            msg_type=MSG_TYPE_SEND_FILE,
            payload_size=len(prefix) + len(body),
        )

        dst = self.out_dir / file_name.decode("ascii")
        self._reset_output_path(dst)

        ready_ack, final_ack = self._send_raw_file_transfer(header, prefix, body)
        self.assertEqual(
            ready_ack[1],
            0,
            f"unexpected ready ack: {ready_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertEqual(
            final_ack[1],
            0,
            f"unexpected final ack: {final_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertTrue(
            wait_for_file_stable(dst, timeout=8.0),
            f"raw empty transfer did not save {dst}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertEqual(b"", dst.read_bytes(), f"raw empty content mismatch for {dst}")
        self._assert_no_temp_files(file_name.decode("ascii"))

    def test_partial_raw_file_transfer_cleans_up_temp_file(self) -> None:
        file_name = b"partial.bin"
        content_size = 1024
        prefix = self._make_file_prefix(file_name, content_size)
        header = self._make_header(
            msg_type=MSG_TYPE_SEND_FILE,
            payload_size=len(prefix) + content_size,
        )

        log_offset = self._server_log_offset()
        ready_ack, final_ack = self._send_raw_file_transfer(header, prefix, b"x" * 100)
        self.assertEqual(
            ready_ack[1],
            0,
            f"unexpected ready ack: {ready_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self.assertEqual(
            final_ack,
            self._make_res_frame(1, 2, 12),
            f"unexpected final ack: {final_ack!r}; server_log_tail={self._server_log_tail()!r}",
        )
        self._wait_for_server_log(
            "protocol error: unexpected EOF while receiving file",
            offset=log_offset,
        )

        final_name = file_name.decode("ascii")
        self.assertFalse((self.out_dir / final_name).exists())
        self._assert_no_temp_files(final_name)

    def test_server_graceful_shutdown_on_signal(self) -> None:
        shared_server = self.__class__.server
        shared_server.stop()

        with make_temp_dir(prefix="hf_transfer_shutdown_") as tmp_dir:
            base_dir = Path(tmp_dir)
            out_dir = base_dir / "outputs"
            out_dir.mkdir(parents=True, exist_ok=True)
            log_path = base_dir / "hf_server_shutdown.log"

            server = HFileServer(
                hf_path=self.hf_path,
                out_dir=out_dir,
                log_path=log_path,
            )
            server.start(startup_timeout=5.0)
            try:
                if os.name == "nt":
                    proc = server.proc
                    proc.terminate()
                    proc.wait(timeout=5.0)
                else:
                    os.kill(server.pid, signal.SIGINT)
                    deadline = time.monotonic() + 5.0
                    while time.monotonic() < deadline:
                        try:
                            os.kill(server.pid, 0)
                        except ProcessLookupError:
                            break
                        time.sleep(0.05)
                    else:
                        self.fail("daemon did not exit after SIGINT")

                log_text = tail_text_file(server.log_path or Path(""))
                if os.name != "nt":
                    self.assertIn("shutdown requested, stopping server", log_text)
                self.assertNotIn("http server stopped unexpectedly", log_text)

                leftovers = [p.name for p in out_dir.iterdir() if ".tmp." in p.name]
                self.assertFalse(
                    leftovers, f"unexpected temp files left after shutdown: {leftovers}"
                )
            finally:
                server.stop()
                shared_server.start(startup_timeout=5.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
