# Repository Guidelines

## Scope

This file is for agentic coding tools working in this repository.
It reflects the current project structure and scripts in `/Users/syh/fun/HFile`.

No extra editor-specific rules were found:
- No `.cursorrules`
- No `.cursor/rules/`
- No `.github/copilot-instructions.md`

## Project Overview

HFile is a lightweight C file-transfer tool with:
- a custom TCP binary protocol for CLI-to-server transfers
- an embedded HTTP/Web UI served by the same server process
- minimal dependencies and explicit platform branches

Core design principles:
- explicit protocol parsing over implicit behavior
- small, native C implementation with few dependencies
- cross-platform support via `#ifdef _WIN32`
- fail fast on invalid input, protocol violations, and I/O errors
- test-driven changes using Python `unittest`

## Repository Layout

```text
src/                    Main C sources and headers
  hfile.c               Program entry point
  cli.c/.h              CLI parsing and help text
  server.c/.h           Listener, per-connection handling, protocol dispatch
  client.c/.h           CLI client send paths
  http.c/.h             HTTP parsing and Web UI/API handling
  protocol.c/.h         Binary protocol encode/decode
  net.c/.h              Socket helpers, send_all/recv_all, endian helpers
  transfer_io.c/.h      Stream file receive-to-disk helpers
  fs.c/.h               File system helpers
  message_store.c/.h    In-memory latest-message store
  webui.c/.h            Embedded HTML/CSS/JS assets

third_party/            Vendored C sources
test/                   Python unittest suites
  test_hf.py            Full/default suite
  test_http.py          HTTP behavior
  test_transfer.py      Protocol and transfer behavior
  test_cli.py           CLI behavior
  test_support.py       Test helper behavior
  support/hf.py         Binary resolution and test helpers

build/                  Generated build output
```

## Build Commands

Preferred build flow:

```bash
./build.sh
```

Other supported builds:

```bash
BUILD_TYPE=Release ./build.sh
./build.sh -t Release
./build.sh -w            # cross-compile for Windows with MinGW
./build.sh -i            # install non-Windows build
```

Direct CMake commands:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cmake --build build --target hf
```

Notes:
- `build.sh` writes `build/` and exports `compile_commands.json`
- The normal local binary is `build/hf`
- A Windows-target build produces `build/hf.exe`

## Test Commands

Default/full suite:

```bash
./test.sh
./test.sh full
python3 -m unittest -v test.test_hf
```

Suite shortcuts:

```bash
./test.sh support
./test.sh cli
./test.sh http
./test.sh transfer
```

Single test file:

```bash
python3 -m unittest -v test.test_http
python3 -m unittest -v test.test_transfer
```

Single test class:

```bash
python3 -m unittest -v test.test_transfer.TestTransfer
```

Single test method:

```bash
python3 -m unittest -v test.test_transfer.TestTransfer.test_reject_mismatched_magic
python3 -m unittest -v test.test_http.TestHTTP.test_upload_list_and_download
```

Passing raw unittest args through the helper script:

```bash
./test.sh -v test.test_transfer.TestTransfer.test_reject_mismatched_magic
```

Test expectations:
- Build before running tests
- Prefer the smallest relevant suite for quick feedback
- Add or update tests for behavior changes, especially protocol, HTTP, and CLI behavior

## Lint / Static Checks

There is no dedicated lint script or formatter in this repo.
Use compiler warnings and tests as the primary validation tools.

Compiler warning policy from `CMakeLists.txt`:
- MSVC: `/W4`
- non-MSVC: `-Wall -Wextra`

When making meaningful C changes, at minimum run:

```bash
cmake --build build
python3 -m unittest -v <relevant test target>
```

## Code Style

### Formatting

- Indent with 2 spaces, never tabs
- Opening braces stay on the same line
- Keep lines readable; soft limit around 100 columns
- Match surrounding style manually; there is no auto-formatter

### Includes

Use this order in `.c` files:
1. matching project header first
2. other project headers
3. standard library headers
4. platform-specific headers inside `#ifdef _WIN32` blocks last

Example:

```c
#include "protocol.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif
```

### Naming

- Functions: `snake_case`
- Variables: `snake_case`
- Files: `snake_case.c` / `snake_case.h`
- Macros/constants: `UPPER_CASE`
- Enum values: `UPPER_CASE`
- Struct typedefs: usually `snake_case_t`
- Internal helpers: `static` and named consistently with nearby code

### Types

- Prefer fixed-width integer types from `stdint.h`
- Use `size_t` for buffer sizes and allocation lengths
- Use `ssize_t` for byte counts returned from send/recv helpers
- Use `socket_t` instead of raw `int`/`SOCKET` in networking code
- Keep protocol field widths exact and compatible with wire format

### Control Flow

- Prefer early returns over deep nesting
- Use `switch` where message types or modes branch naturally
- Use `goto CLEANUP` only when it clearly simplifies shared cleanup
- Keep platform branches explicit and minimal

## Error Handling Rules

- Return `0` on success and non-zero on failure for most C functions
- Check every allocation result
- Check every socket/file I/O return value
- Do not silently ignore partial reads or partial writes
- For fixed-length socket I/O, prefer `send_all` / `recv_all`
- Always check `send_all` / `recv_all` return values:
  - `< 0` means I/O error
  - `< expected` means short read/write or EOF
- Report protocol-layer issues explicitly with existing enums/messages
- Free allocated memory on every exit path

## Socket and Protocol Conventions

- Listener setup lives in `server.c`; shared socket helpers live in `net.c`
- `send_all` / `recv_all` are for fixed-length payloads, headers, ACKs, and similar exact reads/writes
- Do not force `recv_all` into delimiter-based parsing or large streaming file paths
- Large file receives should remain streaming and write incrementally to disk
- Use `encode_u64_be`, `decode_u64_be`, and related helpers for wire encoding
- Preserve the explicit protocol header contract in `protocol.h`

## HTTP and Web UI Notes

- HTTP handling is routed through `http_handle_connection()`
- The active server path is the unified listener in `server.c`, not a separate HTTP-only listener
- Web assets are embedded in `webui.c`; editing them requires rebuilding the binary
- Keep HTTP parsing conservative and explicit; this is a hand-rolled HTTP subset, not a framework

## Testing Guidance by Area

- CLI parsing/help changes: `test/test_cli.py`
- Binary protocol/header changes: `test/test_transfer.py`
- File transfer behavior: `test/test_transfer.py`
- HTTP routes, uploads, messages, Web UI behavior: `test/test_http.py`
- Test helper behavior: `test/test_support.py`

## Agent Guidance

- Prefer small, focused changes over large refactors
- Do not add new dependencies unless clearly necessary
- Avoid changing established protocol behavior without tests
- Preserve cross-platform branches when editing socket or filesystem code
- If removing dead code, remove declarations and related unused types too
- Before finishing, report what was changed and what validation was run
