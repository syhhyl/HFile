from __future__ import annotations

import http.client
import json
import os
import signal
import shutil
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from test.support.hf import (
    HFileServer,
    assert_files_equal,
    make_temp_dir,
    reserve_free_port,
    resolve_hf_path,
    run_hf,
    tail_text_file,
    wait_for_file_stable,
)


class TestHTTP(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            cls.hf_path = resolve_hf_path()
        except FileNotFoundError as e:
            raise unittest.SkipTest(str(e))

        cls._tmp = make_temp_dir(prefix="hf_http_")
        cls.base_dir = Path(cls._tmp.name)
        cls.in_dir = cls.base_dir / "inputs"
        cls.out_dir = cls.base_dir / "outputs"
        cls.log_path = cls.base_dir / "hf_http_server.log"
        cls.in_dir.mkdir(parents=True, exist_ok=True)
        cls.out_dir.mkdir(parents=True, exist_ok=True)

        try:
            port = reserve_free_port()
        except OSError as e:
            raise unittest.SkipTest(str(e))

        cls.server = HFileServer(
            hf_path=cls.hf_path,
            out_dir=cls.out_dir,
            port=port,
            log_path=cls.log_path,
        )
        cls.server.start(startup_timeout=5.0)

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "server"):
            cls.server.stop()
        if hasattr(cls, "_tmp"):
            cls._tmp.cleanup()

    def _request(
        self,
        method: str,
        path: str,
        *,
        data: bytes | None = None,
        headers: dict[str, str] | None = None,
    ) -> tuple[int, bytes, object]:
        req = urllib.request.Request(
            self.server.http_url + path,
            data=data,
            method=method,
            headers=headers or {},
        )
        try:
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                return resp.status, resp.read(), resp.headers
        except urllib.error.HTTPError as e:
            try:
                return e.code, e.read(), e.headers
            finally:
                e.close()

    def _transfer_encoding_request(
        self,
        method: str,
        path: str,
        body: bytes,
        *,
        transfer_encoding: str,
        headers: dict[str, str] | None = None,
    ) -> tuple[int, bytes, object]:
        conn = http.client.HTTPConnection(
            self.server.host, self.server.port, timeout=5.0
        )
        try:
            conn.putrequest(method, path)
            conn.putheader("Transfer-Encoding", transfer_encoding)
            for key, value in (headers or {}).items():
                conn.putheader(key, value)
            conn.endheaders()
            if body:
                conn.send(body)

            resp = conn.getresponse()
            try:
                return resp.status, resp.read(), resp.headers
            finally:
                resp.close()
        finally:
            conn.close()

    def _reset_output_path(self, path: Path) -> None:
        if path.exists():
            if path.is_file() or path.is_symlink():
                path.unlink()
            else:
                shutil.rmtree(path, ignore_errors=True)

    def _read_sse_message(self, resp: http.client.HTTPResponse, expected: str) -> None:
        saw_event = False
        saw_data = False
        while True:
            line = resp.fp.readline()
            if not line:
                self.fail(f"sse stream closed before delivering {expected!r}")
            text = line.decode("utf-8", errors="replace").strip()
            if text == "event: message":
                saw_event = True
            elif text == f"data: {expected}":
                saw_data = True
            elif text == "":
                if saw_event and saw_data:
                    return
                saw_event = False
                saw_data = False

    def test_index_serves_html(self) -> None:
        status, body, _ = self._request("GET", "/")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        text = body.decode("utf-8", errors="replace")
        self.assertIn("HFile", text)
        self.assertIn("Bytes with intent.", text)
        self.assertIn("/app.js", text)
        self.assertIn("Latest Message", text)
        self.assertIn("Current Folder: /", text)

    def test_static_assets_are_served(self) -> None:
        status, body, headers = self._request("GET", "/styles.css")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        self.assertIn("text/css", headers.get_content_type())
        self.assertIn("--bg:", body.decode("utf-8", errors="replace"))

        status, body, headers = self._request("GET", "/app.js")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        self.assertIn("application/javascript", headers.get_content_type())
        self.assertIn("EventSource", body.decode("utf-8", errors="replace"))
        self.assertIn("Current Folder:", body.decode("utf-8", errors="replace"))
        self.assertIn("loadFiles(parentDir(currentDir))", body.decode("utf-8", errors="replace"))

    def test_upload_list_and_download(self) -> None:
        src = self.in_dir / "mobile.txt"
        src.write_bytes(b"hello from mobile web\n")
        dst = self.out_dir / src.name
        self._reset_output_path(dst)

        status, body, _ = self._request(
            "PUT",
            f"/api/files/{urllib.parse.quote(src.name)}",
            data=src.read_bytes(),
            headers={"Content-Type": "application/octet-stream"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))
        self.assertTrue(
            wait_for_file_stable(dst, timeout=5.0),
            f"file not saved: {dst}; server_log_tail={tail_text_file(self.server.log_path or Path(''))!r}",
        )

        status, body, _ = self._request("GET", "/api/files")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        names = [item["name"] for item in payload]
        self.assertIn(src.name, names)
        item = next(item for item in payload if item["name"] == src.name)
        self.assertEqual(item["path"], src.name)
        self.assertEqual(item["kind"], "file")

        status, body, _ = self._request(
            "GET", f"/api/files/{urllib.parse.quote(src.name)}"
        )
        self.assertEqual(status, 200)
        self.assertEqual(body, src.read_bytes())
        assert_files_equal(self, src, dst)

        status, body, _ = self._request(
            "DELETE", f"/api/files/{urllib.parse.quote(src.name)}"
        )
        self.assertEqual(status, 405, body.decode("utf-8", errors="replace"))
        self.assertTrue(dst.exists(), f"file should not be deleted: {dst}")

    def test_upload_rejects_transfer_encoding(self) -> None:
        status, body, _ = self._transfer_encoding_request(
            "PUT",
            "/api/files/transfer-encoding.txt",
            b"",
            transfer_encoding="chunked",
            headers={"Content-Type": "application/octet-stream"},
        )
        self.assertEqual(status, 501, body.decode("utf-8", errors="replace"))

    def test_rejects_invalid_file_name(self) -> None:
        invalid_name = urllib.parse.quote("\\bad.txt", safe="")
        status, body, _ = self._request(
            "GET",
            f"/api/files/{invalid_name}",
        )
        self.assertEqual(status, 400, body.decode("utf-8", errors="replace"))

    def test_nested_http_paths_and_directory_listing(self) -> None:
        nested_dir = self.out_dir / "docs"
        nested_dir.mkdir(parents=True, exist_ok=True)
        nested_name = "docs/readme.txt"
        nested_url = urllib.parse.quote(nested_name, safe="")
        src = self.in_dir / "readme.txt"
        src.write_bytes(b"nested http path\n")
        dst = self.out_dir / nested_name
        self._reset_output_path(dst)

        status, body, _ = self._request(
            "PUT",
            f"/api/files/{nested_url}",
            data=src.read_bytes(),
            headers={"Content-Type": "application/octet-stream"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["name"], "readme.txt")
        self.assertEqual(payload["path"], nested_name)
        self.assertEqual(payload["kind"], "file")
        self.assertTrue(wait_for_file_stable(dst, timeout=5.0))

        status, body, _ = self._request("GET", "/api/files")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        item = next(item for item in payload if item["name"] == "docs")
        self.assertEqual(item["path"], "docs")
        self.assertEqual(item["kind"], "dir")

        status, body, _ = self._request(
            "GET", f"/api/files?path={urllib.parse.quote('docs', safe='')}"
        )
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(len(payload), 1)
        self.assertEqual(payload[0]["name"], "readme.txt")
        self.assertEqual(payload[0]["path"], nested_name)
        self.assertEqual(payload[0]["kind"], "file")

        status, body, _ = self._request("GET", f"/api/files/{nested_url}")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        self.assertEqual(body, src.read_bytes())

        status, body, _ = self._request("DELETE", "/api/files/docs")
        self.assertEqual(status, 405, body.decode("utf-8", errors="replace"))
        self.assertTrue((self.out_dir / "docs").exists())

    def test_root_directory_listing_accepts_symlink_output_dir(self) -> None:
        if os.name == "nt":
            self.skipTest("symlinked output dir coverage is POSIX-only")

        shared_server = self.__class__.server
        shared_server.stop()

        with make_temp_dir(prefix="hf_http_symlink_root_") as tmp_dir:
            base_dir = Path(tmp_dir)
            real_out_dir = base_dir / "real_outputs"
            real_out_dir.mkdir(parents=True, exist_ok=True)
            linked_out_dir = base_dir / "linked_outputs"
            linked_out_dir.symlink_to(real_out_dir, target_is_directory=True)
            (real_out_dir / "through-symlink.txt").write_bytes(b"hello from symlink root\n")

            log_path = base_dir / "hf_http_symlink_root.log"
            port = reserve_free_port()
            server = HFileServer(
                hf_path=self.hf_path,
                out_dir=linked_out_dir,
                port=port,
                log_path=log_path,
            )
            server.start(startup_timeout=5.0)
            try:
                req = urllib.request.Request(server.http_url + "/api/files", method="GET")
                with urllib.request.urlopen(req, timeout=5.0) as resp:
                    self.assertEqual(resp.status, 200)
                    payload = json.loads(resp.read().decode("utf-8"))
                names = [item["name"] for item in payload]
                self.assertIn("through-symlink.txt", names)
            finally:
                server.stop()
                shared_server.start(startup_timeout=5.0)

    def test_message_post_updates_latest_message(self) -> None:
        status, body, _ = self._request(
            "POST",
            "/api/messages",
            data=json.dumps({"message": "hello from browser"}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))

        status, body, _ = self._request("GET", "/api/messages/latest")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["has_message"], True)
        self.assertEqual(payload["message"], "hello from browser")
        self.assertNotIn(
            "msg: hello from browser", tail_text_file(self.server.log_path or Path(""))
        )

    def test_message_post_rejects_transfer_encoding(self) -> None:
        status, body, _ = self._transfer_encoding_request(
            "POST",
            "/api/messages",
            b"",
            transfer_encoding="chunked",
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 501, body.decode("utf-8", errors="replace"))

    def test_message_post_accepts_unicode_escape_sequences(self) -> None:
        status, body, _ = self._request(
            "POST",
            "/api/messages",
            data=b'{"message":"\\u4f60\\u597d"}',
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))

        status, body, _ = self._request("GET", "/api/messages/latest")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["has_message"], True)
        self.assertEqual(payload["message"], "你好")

    def test_message_post_trims_trailing_whitespace_before_store(self) -> None:
        status, body, _ = self._request(
            "POST",
            "/api/messages",
            data=json.dumps({"message": "hello \t\r\n"}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))

        status, body, _ = self._request("GET", "/api/messages/latest")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["has_message"], True)
        self.assertEqual(payload["message"], "hello")

    def test_message_post_all_whitespace_stores_empty_message(self) -> None:
        status, body, _ = self._request(
            "POST",
            "/api/messages",
            data=json.dumps({"message": " \t\r\n"}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))

        status, body, _ = self._request("GET", "/api/messages/latest")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["has_message"], True)
        self.assertEqual(payload["message"], "")

    def test_message_post_trims_trailing_unicode_whitespace_before_store(self) -> None:
        status, body, _ = self._request(
            "POST",
            "/api/messages",
            data=json.dumps({"message": "hello\u3000"}, ensure_ascii=False).encode(
                "utf-8"
            ),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))

        status, body, _ = self._request("GET", "/api/messages/latest")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["has_message"], True)
        self.assertEqual(payload["message"], "hello")

    def test_tcp_message_updates_latest_message(self) -> None:
        r = run_hf(
            self.hf_path,
            [
                "-m",
                "hello from tcp",
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

        status, body, _ = self._request("GET", "/api/messages/latest")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["has_message"], True)
        self.assertEqual(payload["message"], "hello from tcp")
        self.assertNotIn(
            "msg: hello from tcp", tail_text_file(self.server.log_path or Path(""))
        )

    def test_tcp_message_trims_trailing_whitespace_before_store(self) -> None:
        r = run_hf(
            self.hf_path,
            [
                "-m",
                "hello \t",
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

        status, body, _ = self._request("GET", "/api/messages/latest")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["has_message"], True)
        self.assertEqual(payload["message"], "hello")

    def test_tcp_message_all_whitespace_stores_empty_message(self) -> None:
        r = run_hf(
            self.hf_path,
            [
                "-m",
                " \t",
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

        status, body, _ = self._request("GET", "/api/messages/latest")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        payload = json.loads(body.decode("utf-8"))
        self.assertEqual(payload["has_message"], True)
        self.assertEqual(payload["message"], "")

    def test_message_stream_receives_updates(self) -> None:
        conn = http.client.HTTPConnection(
            self.server.host, self.server.port, timeout=5.0
        )
        try:
            conn.request("GET", "/api/messages/stream")
            resp = conn.getresponse()
            self.assertEqual(resp.status, 200)
            self.assertEqual(
                resp.getheader("Content-Type"), "text/event-stream; charset=utf-8"
            )

            status, body, _ = self._request(
                "POST",
                "/api/messages",
                data=json.dumps({"message": "hello from sse"}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
            )
            self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))

            self._read_sse_message(resp, "hello from sse")
        finally:
            conn.close()

    def test_message_stream_broadcasts_to_multiple_clients(self) -> None:
        conns = [
            http.client.HTTPConnection(self.server.host, self.server.port, timeout=5.0),
            http.client.HTTPConnection(self.server.host, self.server.port, timeout=5.0),
        ]
        responses: list[http.client.HTTPResponse] = []
        try:
            for conn in conns:
                conn.request("GET", "/api/messages/stream")
                resp = conn.getresponse()
                self.assertEqual(resp.status, 200)
                self.assertEqual(
                    resp.getheader("Content-Type"), "text/event-stream; charset=utf-8"
                )
                responses.append(resp)

            message = "hello from two sse clients"
            status, body, _ = self._request(
                "POST",
                "/api/messages",
                data=json.dumps({"message": message}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
            )
            self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))

            for resp in responses:
                self._read_sse_message(resp, message)
        finally:
            for conn in conns:
                conn.close()

    def test_http_server_graceful_shutdown_on_signal(self) -> None:
        shared_server = self.__class__.server
        shared_server.stop()

        with make_temp_dir(prefix="hf_http_shutdown_") as tmp_dir:
            base_dir = Path(tmp_dir)
            out_dir = base_dir / "outputs"
            out_dir.mkdir(parents=True, exist_ok=True)
            log_path = base_dir / "hf_http_shutdown.log"
            port = reserve_free_port()

            server = HFileServer(
                hf_path=self.hf_path,
                out_dir=out_dir,
                port=port,
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
                self.assertNotIn("accept(http)", log_text)
            finally:
                server.stop()
                shared_server.start(startup_timeout=5.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
