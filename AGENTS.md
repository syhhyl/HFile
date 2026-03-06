# AGENTS
Guide for agentic coding assistants working in this repository.
If this file conflicts with source code behavior, follow the code.

## Project Overview
- Language: C (portable POSIX + Windows via `_WIN32`)
- Build system: CMake + Ninja
- Binary: `hf` (native); Windows cross-build also produces `hf.exe`
- Core modules:
  - CLI: `src/cli.c`, `src/cli.h`
  - Entry: `src/main.c`
  - Client: `src/client.c`, `src/client.h`
  - Server: `src/server.c`, `src/server.h`
  - Net helpers: `src/net.c`, `src/net.h`
  - File/OS helpers: `src/fs.c`, `src/fs.h`, `src/helper.c`, `src/helper.h`
- Tests: Python `unittest` in `test/test_*.py` (uses `build/hf`)
- Fixtures: `test/fixtures/`

## Build / Lint / Test
### Prerequisites
- `cmake` >= 3.16
- `ninja`
- Python 3.10+

### Build (native)
- Debug build (default): `./build.sh`
- Release build: `BUILD_TYPE=Release ./build.sh`
- Build + install to `$HOME/.local`: `./build.sh --install`
- Custom type: `./build.sh --build-type RelWithDebInfo`

Manual native commands:
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
- Build: `cmake --build build`
- Install: `cmake --install build`

### Build (Windows cross-compile)
- `./build.sh -w`
- `./build.sh --windows`

Notes on `build.sh`:
- Tracks last platform in `.build_platform`
- Removes `build/` when switching native <-> Windows
- Do not run native and cross-build concurrently

### Run
- Server: `./build/hf -s <output_dir> [-p <port>] [--perf]`
- Client: `./build/hf -c <file_path> [-i <ip>] [-p <port>] [--perf]`
- Defaults:
  - Port: `9000`
  - Client IP: `127.0.0.1`

### Tests
Recommended full suite:
- `python3 -m unittest discover -s test -p 'test_*.py' -v`

Run all tests via suite module:
- `python3 -m unittest -v test.test_hf`

Run a single test method (fast loop):
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`

Test behavior notes:
- Tests start an `hf` server subprocess and wait for the log line containing `listening on `.
- The test runner resolves the binary via `test/util_hf.py`; currently it requires `build/hf` to exist.
- Tests reserve free TCP ports; avoid noisy parallel runs.

### Lint / Format
- No formatter configured (keep diffs minimal and consistent with existing style).
- No linter configured.
- Compiler warnings: `-Wall -Wextra`.

## Repository Structure
- `src/`: C sources/headers
- `test/`: Python tests and helpers
- `test/fixtures/`: test payloads (text/binary)
- `build/`: generated build output

## Cursor / Copilot Rules
- No `.cursor/rules/` directory found.
- No `.cursorrules` file found.
- No `.github/copilot-instructions.md` found.

## C Code Style and Conventions
### Formatting
- 2-space indentation, no tabs.
- K&R braces: `if (...) {`.
- Keep functions straightforward and explicit; prefer early return on invalid state.
- Use cleanup labels (`goto`) when multiple resources require release.
- Avoid large stack buffers for transfer data (prefer heap `malloc`).

### Includes
- Use `"..."` for project headers, `<...>` for system headers.
- Keep include order stable and readable.
- Guard platform-specific includes/logic with `_WIN32`.

### Naming
- Files: `lower_snake_case.c` / `.h`.
- Functions/variables: `lower_snake_case`.
- Keep existing enum/type style (e.g. `Mode`, `Opt`, `parse_result_t`).

### Types
- Prefer fixed-width integers from `<stdint.h>` for on-wire/protocol fields.
- `uint16_t`: file name length, port.
- `uint64_t`: on-wire file content size.
- `size_t`: buffer sizes/offsets; `ssize_t`: I/O return values.
- On Windows, `ssize_t` is provided in `src/net.h` for toolchains that lack it.

### Error Handling
- Command-style functions typically return `0` success and `1` failure.
- Usage/validation failures: `fprintf(stderr, ...)`.
- File/syscall failures: `perror(...)` when `errno` is meaningful.
- Socket failures: `sock_perror(...)` (wraps `WSAGetLastError()` on Windows).
- Retry interrupted socket ops on `EINTR` / `WSAEINTR`.

### Resource Management / Portability
- Release all acquired resources on every exit path.
- Use `socket_close(...)` for sockets and `fd_close(...)` for file descriptors.
- Free heap allocations on all paths.
- On Windows, open payload files with `O_BINARY`.

## Networking Protocol (Current)
Single-file transfer frame over one TCP connection:
1) `file_name_len`: 2-byte big-endian `uint16_t`
2) `file_name`: raw bytes, no NUL terminator
3) `file_content_size`: 8-byte big-endian `uint64_t`
4) `file_content`: exactly `file_content_size` bytes

Server-side validation expectations:
- Reject `file_name_len == 0` and `file_name_len > 255`.
- Reject traversal-like names containing `/`, `\\`, or `..`.

Protocol helpers in `src/net.c`:
- `send_all(...)`, `recv_all(...)`, `write_all(...)`
- `encode_u64_be(...)`, `decode_u64_be(...)`

## Debugging Notes
- Debug builds define `DEBUG` in `CMakeLists.txt`.
- `DBG(...)` macro in `src/helper.h` writes to stderr when `DEBUG` is enabled.
- Server readiness log line is `listening on ...` (used by tests).

## Quick Checklist
- Build: `./build.sh`
- Single test: `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Full suite: `python3 -m unittest discover -s test -p 'test_*.py' -v`
