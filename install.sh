#!/usr/bin/env bash
set -euo pipefail

REPO_OWNER="syhhyl"
REPO_NAME="HFile"
BIN_NAME="hf"
API_BASE="https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}"
RELEASE_BASE="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/download"

VERSION=""
INSTALL_DIR=""

usage() {
  cat <<EOF
Usage: install.sh [options]

Download and install HFile from GitHub Releases.

Options:
  --version <tag>  Install a specific release tag, for example v0.0.6
  --dir <path>     Install into a specific directory
  -h, --help       Show this help message
EOF
}

log() {
  printf '%s\n' "$*"
}

fail() {
  printf 'Error: %s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

resolve_version() {
  if [ -n "$VERSION" ]; then
    printf '%s\n' "$VERSION"
    return
  fi

  curl -fsSL "${API_BASE}/releases/latest" |
    sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
    head -n 1
}

detect_os() {
  case "$(uname -s)" in
    Darwin)
      printf 'darwin\n'
      ;;
    Linux)
      printf 'linux\n'
      ;;
    *)
      fail "unsupported operating system: $(uname -s)"
      ;;
  esac
}

detect_arch() {
  case "$(uname -m)" in
    x86_64)
      printf 'amd64\n'
      ;;
    arm64|aarch64)
      printf 'arm64\n'
      ;;
    *)
      fail "unsupported architecture: $(uname -m)"
      ;;
  esac
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
    return
  fi

  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
    return
  fi

  if command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$1" | awk '{print $NF}'
    return
  fi

  fail "no SHA256 tool found (sha256sum, shasum, or openssl)"
}

default_install_dir() {
  if [ -d "/usr/local/bin" ] && [ -w "/usr/local/bin" ]; then
    printf '/usr/local/bin\n'
    return
  fi

  printf '%s/.local/bin\n' "$HOME"
}

ensure_install_dir() {
  if [ ! -d "$1" ]; then
    mkdir -p "$1"
  fi

  [ -d "$1" ] || fail "failed to create install directory: $1"
  [ -w "$1" ] || fail "install directory is not writable: $1"
}

download() {
  local url=$1
  local output=$2

  curl -fL "$url" -o "$output"
}

verify_checksum() {
  local checksum_file=$1
  local archive_name=$2
  local archive_path=$3
  local expected actual

  expected=$(awk -v name="$archive_name" '$2 == name { print $1; exit }' "$checksum_file")
  [ -n "$expected" ] || fail "checksum entry not found for ${archive_name}"

  actual=$(sha256_file "$archive_path")
  [ "$expected" = "$actual" ] || fail "checksum mismatch for ${archive_name}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version)
      [ $# -ge 2 ] || fail "missing value for --version"
      VERSION=$2
      shift 2
      ;;
    --dir)
      [ $# -ge 2 ] || fail "missing value for --dir"
      INSTALL_DIR=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

need_cmd curl
need_cmd tar
need_cmd awk
need_cmd sed
need_cmd head
need_cmd uname
need_cmd mktemp

OS=$(detect_os)
ARCH=$(detect_arch)
TAG=$(resolve_version)
[ -n "$TAG" ] || fail "failed to resolve release version"

if [ -z "$INSTALL_DIR" ]; then
  INSTALL_DIR=$(default_install_dir)
fi
ensure_install_dir "$INSTALL_DIR"

ARCHIVE_NAME="${BIN_NAME}-${OS}-${ARCH}.tar.gz"
CHECKSUMS_NAME="checksums.txt"
ARCHIVE_URL="${RELEASE_BASE}/${TAG}/${ARCHIVE_NAME}"
CHECKSUMS_URL="${RELEASE_BASE}/${TAG}/${CHECKSUMS_NAME}"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

ARCHIVE_PATH="${TMPDIR}/${ARCHIVE_NAME}"
CHECKSUMS_PATH="${TMPDIR}/${CHECKSUMS_NAME}"
EXTRACT_DIR="${TMPDIR}/extract"

log "Installing ${BIN_NAME} ${TAG} for ${OS}-${ARCH}"
log "Download: ${ARCHIVE_URL}"

download "$ARCHIVE_URL" "$ARCHIVE_PATH"
download "$CHECKSUMS_URL" "$CHECKSUMS_PATH"
verify_checksum "$CHECKSUMS_PATH" "$ARCHIVE_NAME" "$ARCHIVE_PATH"

mkdir -p "$EXTRACT_DIR"
tar -xzf "$ARCHIVE_PATH" -C "$EXTRACT_DIR"

BIN_PATH="${EXTRACT_DIR}/${BIN_NAME}"
[ -f "$BIN_PATH" ] || fail "archive does not contain ${BIN_NAME} at the top level"

install -m 755 "$BIN_PATH" "${INSTALL_DIR}/${BIN_NAME}"

log "Installed to ${INSTALL_DIR}/${BIN_NAME}"

case ":$PATH:" in
  *":${INSTALL_DIR}:"*)
    ;;
  *)
    log "${INSTALL_DIR} is not in PATH"
    log "Add it to your shell profile if you want to run ${BIN_NAME} directly"
    ;;
esac

log "Run '${BIN_NAME} --help' to get started"
