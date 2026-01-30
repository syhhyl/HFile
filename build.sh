#!/usr/bin/env sh
set -e

BUILD_TYPE=Debug

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_INSTALL_PREFIX=$HOME/.local



cmake --build build

cmake --install build


