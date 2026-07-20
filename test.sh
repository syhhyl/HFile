#!/usr/bin/env bash
set -e

usage() {
  cat <<EOF
Usage: ./test.sh [suite]
Usage: ./test.sh [ctest args...]

Short suites:
  full         Run all tests (unit + integration)
  unit         Run unit tests only
  integration  Run integration tests only
  cli          Run CLI unit and integration tests
  transfer     Run transfer integration tests only
  protocol     Run protocol integration tests only

Examples:
  ./test.sh
  ./test.sh unit
  ./test.sh integration
  ./test.sh cli
  ./test.sh transfer
  ./test.sh protocol
  ./test.sh --output-on-failure
EOF
}

if [ $# -eq 0 ]; then
  exec ctest --test-dir build --output-on-failure
fi

case "$1" in
  -h|--help)
    usage
    exit 0
    ;;
  full)
    shift
    exec ctest --test-dir build --output-on-failure "$@"
    ;;
  unit)
    shift
    exec ctest --test-dir build -L '^unit$' --output-on-failure "$@"
    ;;
  integration)
    shift
    exec ctest --test-dir build -L '^integration$' --output-on-failure "$@"
    ;;
  cli)
    shift
    exec ctest --test-dir build -L '^cli$' --output-on-failure "$@"
    ;;
  transfer)
    shift
    exec ctest --test-dir build -L '^transfer$' --output-on-failure "$@"
    ;;
  protocol)
    shift
    exec ctest --test-dir build -L '^protocol$' --output-on-failure "$@"
    ;;
esac

exec ctest --test-dir build "$@"
