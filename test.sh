#!/usr/bin/env bash
set -e

usage() {
  cat <<EOF
Usage: ./test.sh [suite]
Usage: ./test.sh [unittest args...]

Short suites:
  full      Run the full documented test suite
  support   Run shared test helper tests
  cli       Run CLI tests
  http      Run HTTP tests
  transfer  Run transfer tests
  perf      Run local benchmark end-to-end (prefer LAN IPv4, fallback localhost)
  perf-server  Run benchmark receive server
  perf-client  Run benchmark send client

Examples:
  ./test.sh
  ./test.sh transfer
  ./test.sh perf --sizes 256MiB --runs 1
  ./test.sh perf-server
  ./test.sh perf-client --server-host 192.168.1.10
  ./test.sh -v test.test_transfer
EOF
}

if [ $# -eq 0 ]; then
  exec python3 -m unittest -v test.test_hf
fi

case "$1" in
  -h|--help)
    usage
    exit 0
    ;;
  full)
    shift
    exec python3 -m unittest -v test.test_hf "$@"
    ;;
  support)
    shift
    exec python3 -m unittest -v test.test_support "$@"
    ;;
  cli)
    shift
    exec python3 -m unittest -v test.test_cli "$@"
    ;;
  http)
    shift
    exec python3 -m unittest -v test.test_http "$@"
    ;;
  transfer)
    shift
    exec python3 -m unittest -v test.test_transfer "$@"
    ;;
  perf)
    shift
    exec python3 test/perf_transfer.py local "$@"
    ;;
  perf-server)
    shift
    exec python3 test/perf_transfer.py server "$@"
    ;;
  perf-client)
    shift
    exec python3 test/perf_transfer.py client "$@"
    ;;
esac

exec python3 -m unittest "$@"
