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
            return e.code, e.read(), e.headers

    def _reset_output_path(self, path: Path) -> None:
        if path.exists():
            if path.is_file() or path.is_symlink():
                path.unlink()
            else:
                shutil.rmtree(path, ignore_errors=True)

    def test_index_serves_html(self) -> None:
        status, body, _ = self._request("GET", "/")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        text = body.decode("utf-8", errors="replace")
        self.assertIn("HFile", text)
        self.assertIn("If you need to move bytes, you'll like HFile.", text)
        self.assertIn("/app.js", text)
        self.assertIn("Latest Message", text)

    def test_static_assets_are_served(self) -> None:
        status, body, headers = self._request("GET", "/styles.css")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        self.assertIn("text/css", headers.get_content_type())
        self.assertIn("--bg:", body.decode("utf-8", errors="replace"))

        status, body, headers = self._request("GET", "/app.js")
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        self.assertIn("application/javascript", headers.get_content_type())
        self.assertIn("EventSource", body.decode("utf-8", errors="replace"))

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

        status, body, _ = self._request(
            "GET", f"/api/files/{urllib.parse.quote(src.name)}"
        )
        self.assertEqual(status, 200)
        self.assertEqual(body, src.read_bytes())
        assert_files_equal(self, src, dst)

        status, body, _ = self._request(
            "DELETE", f"/api/files/{urllib.parse.quote(src.name)}"
        )
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        self.assertFalse(dst.exists(), f"file not deleted: {dst}")

    def test_rejects_invalid_file_name(self) -> None:
        invalid_name = urllib.parse.quote("\\bad.txt", safe="")
        status, body, _ = self._request(
            "GET",
            f"/api/files/{invalid_name}",
        )
        self.assertEqual(status, 400, body.decode("utf-8", errors="replace"))

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

            saw_event = False
            saw_data = False
            while True:
                line = resp.fp.readline()
                if not line:
                    self.fail("sse stream closed before delivering message")
                text = line.decode("utf-8", errors="replace").strip()
                if text == "event: message":
                    saw_event = True
                elif text == "data: hello from sse":
                    saw_data = True
                elif text == "":
                    if saw_event and saw_data:
                        break
                    saw_event = False
                    saw_data = False
        finally:
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
