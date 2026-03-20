from __future__ import annotations

import sys
import unittest
from pathlib import Path

TEST_MODULES = [
    "test.test_support",
    "test.test_cli",
    "test.test_http",
    "test.test_transfer",
]


def run_all() -> int:
    root = Path(__file__).resolve().parents[1]
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))

    loader = unittest.TestLoader()
    runner = unittest.TextTestRunner(verbosity=2)

    for mod_name in TEST_MODULES:
        suite = loader.loadTestsFromName(mod_name)
        result = runner.run(suite)
        if not result.wasSuccessful():
            return 1

    return 0


def load_tests(loader: unittest.TestLoader, tests, pattern):
    if pattern is not None:
        return tests

    root = Path(__file__).resolve().parents[1]
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))

    suite = unittest.TestSuite()
    for mod_name in TEST_MODULES:
        suite.addTests(loader.loadTestsFromName(mod_name))
    return suite


if __name__ == "__main__":
    raise SystemExit(run_all())
