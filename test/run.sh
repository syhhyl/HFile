#!/usr/bin/env sh
set -eu

python3 -m unittest discover -s test -p 'test_*.py' -v
