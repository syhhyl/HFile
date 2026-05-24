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
  cli          Run integration tests (CLI + transfer + protocol)
  transfer     Run integration tests (CLI + transfer + protocol)

Examples:
  ./test.sh
  ./test.sh unit
  ./test.sh integration
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
    exec ctest --test-dir build -R unit --output-on-failure "$@"
    ;;
  integration)
    shift
    exec ctest --test-dir build -R integration --output-on-failure "$@"
    ;;
  cli|transfer)
    shift
    exec ctest --test-dir build -R integration --output-on-failure "$@"
    ;;
esac

exec ctest --test-dir build "$@"
