#!/usr/bin/env sh
#==============================================================================
# build.sh - HFile Build Script
#==============================================================================
#
# Cross-platform build script for HFile.
# Uses Ninja as the build generator.
#
# Usage:
#   ./build.sh [OPTIONS]
#
# Options:
#   -m, --macos     Build for macOS (default)
#   -l, --linux     Build for Linux
#   -w, --windows   Cross-compile for Windows (requires mingw-w64)
#   -r, --release   Build in Release mode (default: Debug)
#   -h, --help      Show this help message
#
# Examples:
#   ./build.sh                  # Debug build for current platform
#   ./build.sh --release        # Release build for current platform
#   ./build.sh --windows       # Cross-compile for Windows
#   ./build.sh -w -r           # Windows Release build
#
# Requirements:
#   - CMake >= 3.16
#   - Ninja build system
#   - For Windows cross-compile: mingw-w64 (brew install mingw-w64)
#==============================================================================

set -e

#------------------------------------------------------------------------------
# Print usage information
#------------------------------------------------------------------------------
usage() {
  echo "Usage: $0 [OPTIONS]"
  echo ""
  echo "Options:"
  echo "  -m, --macos     Build for macOS (default)"
  echo "  -l, --linux     Build for Linux"
  echo "  -w, --windows   Cross-compile for Windows (requires mingw-w64)"
  echo "  -r, --release   Build in Release mode (default: Debug)"
  echo "  -h, --help      Show this help message"
  echo ""
  echo "Examples:"
  echo "  $0                  # Debug build for current platform"
  echo "  $0 --release        # Release build for current platform"
  echo "  $0 --windows       # Cross-compile for Windows"
  echo "  $0 -w -r           # Windows Release build"
  exit 1
}

#------------------------------------------------------------------------------
# Parse command-line arguments
#------------------------------------------------------------------------------
PLATFORM=""
BUILD_TYPE="Debug"

while [[ $# -gt 0 ]]; do
  case $1 in
    -m|--macos)
      PLATFORM="macos"
      shift
      ;;
    -l|--linux)
      PLATFORM="linux"
      shift
      ;;
    -w|--windows)
      PLATFORM="windows"
      shift
      ;;
    -r|--release)
      BUILD_TYPE="Release"
      shift
      ;;
    -h|--help)
      usage
      ;;
    *)
      echo "Unknown option: $1"
      usage
      ;;
  esac
done

#------------------------------------------------------------------------------
# Configure CMake options based on platform
#------------------------------------------------------------------------------
CMAKE_OPTS="-DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"

case "$PLATFORM" in
  windows)
    CMAKE_OPTS="$CMAKE_OPTS -DCROSS_COMPILE_WINDOWS=ON"
    ;;
  linux)
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_SYSTEM_NAME=Linux"
    ;;
  macos|"")
    # Native build (default)
    ;;
esac

#------------------------------------------------------------------------------
# Determine install prefix based on platform
#------------------------------------------------------------------------------
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    # Windows (Git Bash, MSYS2, Cygwin)
    INSTALL_PREFIX="$USERPROFILE/.local"
    ;;
  *)
    # macOS, Linux, etc.
    INSTALL_PREFIX="$HOME/.local"
    ;;
esac

CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX"

#------------------------------------------------------------------------------
# Build
#------------------------------------------------------------------------------
echo "=========================================="
echo "  HFile Build Script"
echo "=========================================="
echo "Platform: ${PLATFORM:-native}"
echo "Build type: $BUILD_TYPE"
echo "Install prefix: $INSTALL_PREFIX"
echo "=========================================="
echo ""

# Clean build directory
rm -rf build

# Configure
cmake -S . -B build -G Ninja $CMAKE_OPTS

# Build
cmake --build build

# Install
cmake --install build

#------------------------------------------------------------------------------
# Summary
#------------------------------------------------------------------------------
echo ""
echo "=========================================="
echo "  Build Complete!"
echo "=========================================="
if [ -f "build/hf.exe" ]; then
  echo "Windows executable: build/hf.exe"
  file build/hf.exe
elif [ -f "build/hf" ]; then
  echo "Binary: build/hf"
  file build/hf
fi
echo "=========================================="
