from __future__ import annotations

import json
import shutil
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
            http_port = reserve_free_port()
        except OSError as e:
            raise unittest.SkipTest(str(e))

        cls.server = HFileServer(
            hf_path=cls.hf_path,
            out_dir=cls.out_dir,
            http_port=http_port,
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
        self.assertNotIn("<h2>Messages</h2>", text)

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
            f"file not saved: {dst}; server_log_tail={tail_text_file(self.log_path)!r}",
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

    def test_message_posts_to_server_stdout(self) -> None:
        status, body, _ = self._request(
            "POST",
            "/api/messages",
            data=json.dumps({"message": "hello from browser"}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))
        self.assertIn(
            "msg: hello from browser",
            tail_text_file(self.log_path),
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
