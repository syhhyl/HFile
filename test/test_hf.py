from __future__ import annotations

import sys
import unittest
from pathlib import Path

TEST_FILES = [
  "test_cli",
  # 后续加文件就追加在这里，比如：
  # "test_transfer.py",
  # "test_protocol.py",
]




def Test_All() -> int:

  loader = unittest.TestLoader()
  runner = unittest.TextTestRunner(verbosity=2)

  for filename in TEST_FILES:
    suite = loader.loadTestsFromName(filename)
    result = runner.run(suite)
    if not result.wasSuccessful():
      return 1

  return 0


if __name__ == "__main__":
  raise SystemExit(Test_All())
  