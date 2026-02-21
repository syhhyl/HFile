#!/usr/bin/env sh
set -e

BUILD_TYPE=${BUILD_TYPE:-Debug}
USE_WINDOWS=0

while [ $# -gt 0 ]; do
  case "$1" in
    -w|--windows)
      USE_WINDOWS=1
      ;;
    -h|--help)
      echo "Usage: $0 [-w|--windows]"
      echo "  -w, --windows  Cross-compile for Windows using MinGW"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      echo "Usage: $0 [-w|--windows]"
      exit 1
      ;;
  esac
  shift
done

LAST_PLATFORM_FILE=.build_platform

if [ -f "$LAST_PLATFORM_FILE" ]; then
    LAST_PLATFORM=$(cat "$LAST_PLATFORM_FILE")
else
    LAST_PLATFORM=""
fi

if [ "$USE_WINDOWS" = "1" ]; then
    CURRENT_PLATFORM="Windows"
else
    CURRENT_PLATFORM=$(uname -s)
fi

if [ "$CURRENT_PLATFORM" != "$LAST_PLATFORM" ] && [ -d build ]; then
    echo "Platform changed, cleaning build directory..."
    rm -rf build
fi

echo "$CURRENT_PLATFORM" > "$LAST_PLATFORM_FILE"

CMAKE_OPTS=(
  -S . -B build
  -G Ninja
  -DCMAKE_BUILD_TYPE=$BUILD_TYPE
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DCMAKE_INSTALL_PREFIX=$HOME/.local
)

if [ "$USE_WINDOWS" = "1" ]; then
  CMAKE_OPTS+=(
    -DCMAKE_SYSTEM_NAME=Windows
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc
  )
fi

cmake "${CMAKE_OPTS[@]}"
cmake --build build
cmake --install build