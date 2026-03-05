# AGENTS
Guide for agentic coding assistants working in this repository.
If this file conflicts with source code behavior, follow the code.

## Project Overview
- Language: C
- Build system: CMake + Ninja
- Binary: `hf` (native), `hf.exe` (Windows cross-build)
- Main source files:
  - `src/main.c`
  - `src/cli.c`
  - `src/server.c`
  - `src/client.c`
  - `src/net.c`
  - `src/fs.c`
  - `src/helper.c`
- Tests: Python `unittest` in `test/test_*.py`
- Fixtures: `test/fixtures/`

## Build / Lint / Test
### Prerequisites
- `cmake` >= 3.16
- `ninja`
- Python 3.10+

### Build (native)
- Debug build: `./build.sh`
- Release build: `BUILD_TYPE=Release ./build.sh`
- Build + install: `./build.sh --install`

Manual native commands:
- Configure:
  - `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
- Build:
  - `cmake --build build`
- Install:
  - `cmake --install build`

### Build (Windows cross-compile)
- `./build.sh -w`
- `./build.sh --windows`

Notes on `build.sh`:
- Tracks last platform in `.build_platform`
- Removes `build/` when switching native <-> Windows
- Do not run native and cross-build concurrently

### Run
- Server mode: `hf -s <output_dir> [-p <port>]`
- Client mode: `hf -c <file_path> [-i <ip>] [-p <port>]`
- Defaults from CLI parser:
  - Port: `9000`
  - Client IP: `127.0.0.1`
- Example server: `./build/hf -s ./output -p 9001`
- Example client: `./build/hf -c ./input/hello.txt -i 127.0.0.1 -p 9001`

### Test commands
- All tests (recommended):
  - `python3 -m unittest discover -s test -p 'test_*.py' -v`
- All tests via suite entry:
  - `python3 -m unittest -v test.test_hf`
- Single test method (fastest loop):
  - `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Single CLI test method:
  - `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`

Test behavior notes:
- Tests spawn an `hf` server subprocess and wait for `listening on ...`
- Binary resolution order (`test/util_hf.py`):
  1) `$HF_BIN`
  2) `$HOME/.local/bin/hf`
  3) `build/hf`
- Tests reserve free TCP ports; avoid noisy parallel runs

### Lint / Format
- No standalone formatter configured
- No standalone linter configured
- Compiler warnings enabled in CMake: `-Wall -Wextra`

## Repository Structure
- `src/`: C source and headers
- `test/`: Python tests, fixtures, and helpers
- `build/`: generated files
- `README.md`: short project intro

## Cursor / Copilot Rules
- No `.cursor/rules/` directory found
- No `.cursorrules` file found
- No `.github/copilot-instructions.md` found

## C Code Style and Conventions
### Formatting
- 2-space indentation, no tabs
- K&R braces: `if (...) {`
- Keep functions straightforward and explicit
- Prefer early return on invalid state
- Use cleanup labels (`goto`) when multiple resources require release
- Avoid large stack buffers for transfer data

### Includes
- Use `"..."` for project headers
- Use `<...>` for system headers
- Keep include order stable and readable
- Keep platform-specific includes in `_WIN32` conditionals

### Naming
- Files: `lower_snake_case.c` / `.h`
- Functions: `lower_snake_case`
- Variables: `lower_snake_case`
- Keep existing enum/type style (e.g. `Mode`, `Opt`, `parse_result_t`)

### Types
- Use fixed-width protocol integer types (`stdint.h`)
- `uint16_t`: filename length and port
- `uint64_t`: file content size in protocol
- `size_t`: buffer lengths/offsets
- `ssize_t`: I/O return values

### Error Handling
- Command-style functions return `0` success, `1` failure
- Validation/usage failures: `fprintf(stderr, ...)`
- File/syscall failures: `perror(...)` where `errno` is meaningful
- Socket failures: `sock_perror(...)`
- Retry interrupted socket ops on `EINTR` / `WSAEINTR`

### Resource Management and Portability
- Release all acquired resources on every exit path
- Use `socket_close(...)` for sockets
- Use `fd_close(...)` for file descriptors
- Free heap allocations on all error and success paths
- On Windows, open payload files with `O_BINARY`

## Networking Protocol (Current)
Single-file transfer frame over one TCP connection:
1) `file_name_len`: 2-byte big-endian `uint16_t`
2) `file_name`: raw bytes, no NUL terminator
3) `file_content_size`: 8-byte big-endian `uint64_t`
4) `file_content`: exactly `file_content_size` bytes

Server-side validation expectations:
- Reject `file_name_len == 0` and `file_name_len > 255`
- Reject traversal-like names containing `/`, `\\`, or `..`

Relevant helpers in `src/net.c`:
- `send_all(...)`
- `recv_all(...)`
- `write_all(...)`
- `encode_u64_be(...)`
- `decode_u64_be(...)`

## Debugging Notes
- Debug builds define `DEBUG` in `CMakeLists.txt`
- `DBG(...)` macro is defined in `src/helper.h`
- Server readiness line is `listening on ...` (used by tests)

## Quick Checklist
- Build: `./build.sh`
- Single test: `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Full suite: `python3 -m unittest discover -s test -p 'test_*.py' -v`
