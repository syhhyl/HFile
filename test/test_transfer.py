from __future__ import annotations

import shutil
import unittest
from pathlib import Path

from test.util_hf import (
    HFileServer,
    assert_files_equal,
    make_temp_dir,
    resolve_hf_path,
    run_hf,
    tail_text_file,
    wait_for_file_stable,
)


class TestTransfer(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            cls.hf_path = resolve_hf_path()
        except FileNotFoundError as e:
            raise unittest.SkipTest(str(e))

        cls._tmp = make_temp_dir(prefix="hf_transfer_")
        cls.base_dir = Path(cls._tmp.name)
        cls.in_dir = cls.base_dir / "HFileTest_IN"
        cls.out_dir = cls.base_dir / "HFileTest_OUT"
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

    def test_common_file(self) -> None:
        src = self.in_dir / "hello.txt"
        src.write_bytes(b"hello hfile\n")
        dst = self._send_and_assert_ok(src)
        assert_files_equal(self, src, dst)

    def test_empty_file(self) -> None:
        src = self.in_dir / "empty.txt"
        src.write_bytes(b"")
        dst = self._send_and_assert_ok(src)
        assert_files_equal(self, src, dst)

    def test_fixtures(self) -> None:
        fixtures_dir = Path(__file__).resolve().parent / "fixtures"
        if not fixtures_dir.exists():
            self.skipTest(f"fixtures dir not found: {fixtures_dir}")

        fixtures = sorted(
            [
                p
                for p in fixtures_dir.iterdir()
                if p.is_file() and not p.name.startswith(".")
            ],
            key=lambda p: p.name,
        )
        if not fixtures:
            self.skipTest(f"no fixtures found in: {fixtures_dir}")

        for fixture in fixtures:
            with self.subTest(fixture=fixture.name):
                src = self.in_dir / fixture.name
                shutil.copy2(fixture, src)
                dst = self._send_and_assert_ok(src)
                assert_files_equal(self, src, dst)


if __name__ == "__main__":
    unittest.main(verbosity=2)
