from __future__ import annotations

import unittest
from pathlib import Path

from test.support.hf import (
    make_temp_dir,
    protocol_define,
    resolve_hf_path,
    wait_for_text_in_file,
)


class TestSupport(unittest.TestCase):
    def test_wait_for_text_in_file_finds_existing_text(self) -> None:
        with make_temp_dir(prefix="hf_support_") as tmp_dir:
            path = Path(tmp_dir) / "server.log"
            path.write_text("alpha\nbeta\n", encoding="utf-8")

            text = wait_for_text_in_file(path, "beta", timeout=0.1)

        self.assertIsNotNone(text)
        self.assertIn("beta", text or "")

    def test_wait_for_text_in_file_returns_none_on_timeout(self) -> None:
        with make_temp_dir(prefix="hf_support_") as tmp_dir:
            path = Path(tmp_dir) / "server.log"
            path.write_text("alpha\n", encoding="utf-8")

            text = wait_for_text_in_file(path, "beta", timeout=0.05, interval=0.01)

        self.assertIsNone(text)

    def test_protocol_define_reads_protocol_header(self) -> None:
        self.assertEqual(protocol_define("HF_PROTOCOL_MAGIC"), 0x0429)
        self.assertGreater(protocol_define("HF_PROTOCOL_VERSION"), 0)
        self.assertEqual(protocol_define("HF_MSG_FLAG_NONE"), 0x00)
        self.assertEqual(
            protocol_define("HF_PROTOCOL_MAX_TEXT_MESSAGE_SIZE"), 256 * 1024
        )

    def test_protocol_define_raises_when_symbol_missing(self) -> None:
        with self.assertRaises(KeyError):
            protocol_define("HF_DOES_NOT_EXIST")

    def test_resolve_hf_path_prefers_native_binary(self) -> None:
        with make_temp_dir(prefix="hf_support_") as tmp_dir:
            root = Path(tmp_dir)
            build_dir = root / "build"
            build_dir.mkdir(parents=True, exist_ok=True)
            native_bin = build_dir / "hf"
            windows_bin = build_dir / "hf.exe"
            native_bin.write_text("native\n", encoding="utf-8")
            windows_bin.write_text("windows\n", encoding="utf-8")

            resolved = resolve_hf_path(root, target_os="posix")

        self.assertEqual(resolved, native_bin)

    def test_resolve_hf_path_accepts_windows_binary_name(self) -> None:
        with make_temp_dir(prefix="hf_support_") as tmp_dir:
            root = Path(tmp_dir)
            build_dir = root / "build"
            build_dir.mkdir(parents=True, exist_ok=True)
            windows_bin = build_dir / "hf.exe"
            windows_bin.write_text("windows\n", encoding="utf-8")

            resolved = resolve_hf_path(root, target_os="windows")

        self.assertEqual(resolved, windows_bin)

    def test_resolve_hf_path_raises_when_binary_missing(self) -> None:
        with make_temp_dir(prefix="hf_support_") as tmp_dir:
            root = Path(tmp_dir)
            with self.assertRaises(FileNotFoundError):
                resolve_hf_path(root, target_os="posix")

    def test_resolve_hf_path_does_not_use_windows_binary_on_posix(self) -> None:
        with make_temp_dir(prefix="hf_support_") as tmp_dir:
            root = Path(tmp_dir)
            build_dir = root / "build"
            build_dir.mkdir(parents=True, exist_ok=True)
            (build_dir / "hf.exe").write_text("windows\n", encoding="utf-8")

            with self.assertRaises(FileNotFoundError):
                resolve_hf_path(root, target_os="posix")


if __name__ == "__main__":
    unittest.main(verbosity=2)
