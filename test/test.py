import unittest
from pathlib import Path
import subprocess
import time
import shutil
import hashlib


class Helper:

  @staticmethod
  def create_test_dir(base: Path) -> tuple[Path, Path]:
    in_dir = base / "HFileTest_IN"
    out_dir = base / "HFileTest_OUT"
    in_dir.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    return in_dir, out_dir

  @staticmethod
  def make_file(
      dir_path: Path, filename: str, data: bytes = b"", src_path: Path | None = None
  ) -> Path:
    dst = dir_path / filename
    if src_path is not None:
      shutil.copy2(src_path, dst)
      return dst
    dst.write_bytes(data)
    return dst

  @staticmethod
  def run_client(
      cmd: list,
      check: bool = True,
      timeout: int = 5,
  ) -> subprocess.CompletedProcess:

    return subprocess.run(
      cmd,
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      timeout=timeout,
      check=check,
      text=True,
    )

  @staticmethod
  def wait_for_file(
      path: Path, timeout: float = 2.0, interval: float = 0.05, stable_rounds: int = 3
  ) -> bool:
    deadline = time.time() + timeout
    last_size = -1
    stable = 0
    while time.time() < deadline:
      if path.exists() and path.is_file():
        size = path.stat().st_size
        if size == last_size:
          stable += 1
          if stable >= stable_rounds:
            return True
        else:
          stable = 0
          last_size = size
      time.sleep(interval)
    return False

  @staticmethod
  def sha256_file(p: Path, chunk_size: int = 1024 * 1024) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
      for chunk in iter(lambda: f.read(chunk_size), b""):
        h.update(chunk)
    return h.hexdigest()

  @staticmethod
  def assert_files_equal(
    testcase, src: Path, dst: Path, threshold: int = 4 * 1024 * 1024):
    s1 = src.stat().st_size
    s2 = dst.stat().st_size
    testcase.assertEqual(s1, s2, f"size mismatch: {s1} != {s2}")

    if s1 <= threshold:
      testcase.assertEqual(
        src.read_bytes(), dst.read_bytes(), "content mismatch (bytes)"
      )
    else:
      testcase.assertEqual(
        Helper.sha256_file(src),
        Helper.sha256_file(dst),
        "content mismatch (sha256)",
      )

  @staticmethod
  def assert_sha256_value(src: Path, dst: Path):
    src_sha256 = hashlib.sha256(Path(src).read_bytes()).hexdigest()
    dst_sha256 = hashlib.sha256(Path(dst).read_bytes()).hexdigest()

    print(f"src:{src_sha256}\ndst:{dst_sha256}")


class TestHFile(unittest.TestCase):
  @classmethod
  def setUpClass(cls) -> None:
    base = Path(__file__).resolve().parent
    cls.in_dir, cls.out_dir = Helper.create_test_dir(base)

    cls.hf_path = Path.home() / ".local" / "bin" / "hf"
    if not cls.hf_path.exists():
      cls.hf_path = Path("build/hf")
    
    cls.ip = "127.0.0.1"
    cls.port = "9000"

    cls.server_cmd = [
      str(cls.hf_path),
      "-s",
      str(cls.out_dir),
      "-p", cls.port,
    ]

    cls.server = subprocess.Popen(
      cls.server_cmd,
      stdout=subprocess.DEVNULL,
      stderr=subprocess.DEVNULL,
    )

    if cls.server.poll() is not None:
      raise RuntimeError(
        f"server start failed, exit code: {cls.server.returncode}"
      )


  @classmethod
  def tearDownClass(cls):
    if cls.server.poll() is None:
      cls.server.terminate()
      try:
        cls.server.wait(timeout=3)
      except subprocess.TimeoutExpired:
        cls.server.kill()
        cls.server.wait()

  def run_one_case(self, case: list, msg: str, check=False, returncode=0):
    with self.subTest(args=case):
      cmd = [str(self.hf_path), *case]
      p = Helper.run_client(cmd, check=check)
      self.assertEqual(p.returncode, returncode)
      self.assertIn(msg, p.stderr)
          

  def test_fixtures(self):
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

    for src in fixtures:
      with self.subTest(fixture=src.name):
        dst = self.out_dir / src.name
        if dst.exists():
          if dst.is_file() or dst.is_symlink():
            dst.unlink()
          else:
            shutil.rmtree(dst, ignore_errors=True)

        cmd = [
          str(self.hf_path),
          "-c", src,
        ]
        Helper.run_client(cmd)

        self.assertTrue(
          Helper.wait_for_file(dst, timeout=5.0),
          f"file no save: {dst} (fixture: {src})",
        )
        Helper.assert_files_equal(self, src, dst)

  def test_common_file(self):
    data = b"hello hfile\n"
    src = Helper.make_file(self.in_dir, "hello.txt", data)
    dst = self.out_dir / src.name

    cmd = [
      str(self.hf_path),
      "-c", src,
    ]
    Helper.run_client(cmd)
    self.assertTrue(Helper.wait_for_file(dst), f"file no save: {dst}")
    Helper.assert_files_equal(self, src, dst)

  def test_empty_file(self):
    data = b""
    src = Helper.make_file(self.in_dir, "empty.txt", data)
    dst = self.out_dir / src.name

    cmd = [
      str(self.hf_path),
      "-c", src
    ]
    Helper.run_client(cmd)

    self.assertTrue(Helper.wait_for_file(dst), f"file no save: {dst}")
    Helper.assert_files_equal(self, src, dst)

  def test_large_file(self):
    src = self.in_dir / "largefile.txt"
    dst = self.out_dir / src.name

    with src.open("wb") as f:
      f.truncate(2 * 1024**3)

    cmd = [
      str(self.hf_path),
      "-c", src
    ]
    Helper.run_client(cmd)
    self.assertTrue(Helper.wait_for_file(dst), f"file no save: {dst}")
    Helper.assert_files_equal(self, src, dst)
  
  def test_cli_argument(self):
    

    
    case1 = [
      ["-c", "in", "-s", "out"],
      ["-s", "out", "-c", "in"]
    ]
    
    case2 = [
      ["a"],
      ["aa"]
    ]

    help_case = ["-h"]

    invalid_server_path = [
      ["-s"],
      ["-s", "-c"]
    ]
    invalid_client_path = [
      ["-c"],
      ["-c", "-s"]
    ]
    
    server_no_ip = ["-s", "out", "-i", "10.0.0.1"]

    invalid_port_case = [
      ["-s", "out", "-p"],
      ["-s", "out", "-p", "hfile"],
      ["-c", "in", "-p", "hfile"]
    ]
    
    # -h
    self.run_one_case(help_case, "usage", returncode=0) 

    # use -s -c together
    for a in case1:
      self.run_one_case(a, "cannot use -s -c together", returncode=1)

    # invalid path
    for a in invalid_server_path:
      self.run_one_case(a, "invalid server path", returncode=1)

    for a in invalid_client_path:
      self.run_one_case(a, "invalid client path", returncode=1)

    # server no ip
    self.run_one_case(server_no_ip, "server mode don't need ip", returncode=1)
    
    for a in invalid_port_case:
      self.run_one_case(a, "invalid port", returncode=1)
        
    
    for a in case2:
      self.run_one_case(a, "invalid argument", returncode=1)

if __name__ == "__main__":
  unittest.main(verbosity=2)
