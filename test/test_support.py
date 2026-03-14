from __future__ import annotations

import unittest
from pathlib import Path

from test.support.hf import make_temp_dir, resolve_hf_path


class TestSupport(unittest.TestCase):
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
