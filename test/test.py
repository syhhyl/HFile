import unittest
import tempfile
from pathlib import Path
import subprocess
import time


class Helper:
  @staticmethod
  def create_test_dir(base):
    in_dir = base / "HFileTest_IN"
    out_dir = base / "HFileTest_OUT"
    in_dir.mkdir()
    out_dir.mkdir()
    return in_dir, out_dir
     
  


class TestHFile(unittest.TestCase):
  @classmethod
  def setUpClass(cls) -> None:
    cls.tmp = tempfile.TemporaryDirectory()
    base = Path(cls.tmp.name)
    cls.in_dir, cls.out_dir = Helper.create_test_dir(base)
    cls.server_cmd = [
      "hf",
      "-s", str(cls.out_dir),
      "-p", "9000",
    ]
    
    cls.server = subprocess.Popen(
      cls.server_cmd,
      stdout = subprocess.PIPE,
      stderr = subprocess.PIPE,

    )
    
    time.sleep(0.5)
    print("setup finish")

  @classmethod
  def tearDownClass(cls):
    cls.server.terminate()
    cls.server.wait(timeout=3)
    cls.tmp.cleanup()
    print("teardown")



  def test_basic_send(self):
    print("[test_basic_send] strart")
    src = self.in_dir / "hello.txt"
    data = b"hello hfile\n"
    src.write_bytes(data)

    client_cmd = [
      "hf",
      "-c", src,
      "-p", "9000",
    ]
    
    subprocess.check_call(client_cmd)


if __name__ == "__main__":
  unittest.main(verbosity=2)


    