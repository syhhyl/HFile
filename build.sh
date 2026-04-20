#!/usr/bin/env bash
set -e

BUILD_TYPE=${BUILD_TYPE:-Debug}
DO_INSTALL=0

usage() {
  cat <<EOF
Usage: $0 [options]

Configure and build the project with CMake + Ninja.

Options:
  -t, --target <type>  Set CMake build type (default: $BUILD_TYPE)
  -i, --install        Install after a successful build
  -h, --help           Show this help message

Examples:
  $0
  BUILD_TYPE=Release $0
  $0 -t Release
  $0 -i
EOF
}

need_value() {
  if [ -z "${2:-}" ]; then
    echo "Missing value for $1" >&2
    usage >&2
    exit 1
  fi
  printf '%s' "$2"
}

while [ $# -gt 0 ]; do
  case "$1" in
    -i|--install)
      DO_INSTALL=1
      ;;
    -t|--target)
      BUILD_TYPE=$(need_value "$1" "${2:-}")
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

CMAKE_OPTS=(
  -S . -B build
  -G Ninja
  -DCMAKE_BUILD_TYPE=$BUILD_TYPE
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DCMAKE_INSTALL_PREFIX=$HOME/.local
)

cmake "${CMAKE_OPTS[@]}"
cmake --build build

if [ "$DO_INSTALL" = "1" ]; then
  cmake --install build
fi
