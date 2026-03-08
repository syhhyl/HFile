# AGENTS
Guide for agentic coding assistants working in this repository.
If these notes conflict with the code or tests, follow the code and keep diffs minimal.

## Project Summary
- Language: C
- Portability target: POSIX + Windows via `_WIN32`
- Build system: CMake + Ninja, with `build.sh` as the main entrypoint
- Main binary: `hf` (`hf.exe` for Windows cross-builds)
- Scope: single-file transfer over one TCP connection

## Key Source Files
- Entry point: `src/main.c`
- CLI: `src/cli.c`, `src/cli.h`
- Client/server: `src/client.c`, `src/client.h`, `src/server.c`, `src/server.h`
- Protocol: `src/protocol.c`, `src/protocol.h`
- Platform helpers: `src/net.c`, `src/net.h`, `src/fs.c`, `src/fs.h`
- Save/path/perf helpers: `src/save.c`, `src/save.h`, `src/helper.c`, `src/helper.h`
- Tests: `test/test_*.py`; helpers: `test/util_hf.py`

## Build Commands
### Native builds
- Default debug build: `./build.sh`
- Release build: `BUILD_TYPE=Release ./build.sh`
- Build and install to `$HOME/.local`: `./build.sh --install`
- Custom build type: `./build.sh --build-type RelWithDebInfo`

### Manual native commands
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
- Build: `cmake --build build`
- Install: `cmake --install build`

### Windows cross-build
- `./build.sh -w`
- `./build.sh --windows`

### Build notes
- `build.sh` tracks the active platform in `.build_platform` and recreates `build/` when switching native vs Windows.
- Do not run native and Windows builds concurrently against the same `build/` directory.

## Run Commands
- Server: `./build/hf -s <output_dir> [-p <port>] [--perf]`
- Client: `./build/hf -c <file_path> [-i <ip>] [-p <port>] [--perf] [--compress]`
- Default port: `9000`
- Default client IP: `127.0.0.1`

## Test Commands
### Recommended full suite
- `python3 -m unittest discover -s test -p 'test_*.py' -v`

### Suite module
- `python3 -m unittest -v test.test_hf`

### Single test targets
- File: `python3 -m unittest -v test.test_transfer`
- File: `python3 -m unittest -v test.test_cli`
- Class: `python3 -m unittest -v test.test_transfer.TestTransfer`
- Class: `python3 -m unittest -v test.test_cli.TestCLI`
- Method: `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Method: `python3 -m unittest -v test.test_transfer.TestTransfer.test_common_file`
- Method: `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`

### Test behavior notes
- Tests expect `build/hf` to exist; build before running tests.
- Transfer tests start a real `hf` server subprocess.
- Server readiness is detected from the log line containing `listening on `.
- Tests reserve free TCP ports dynamically; avoid noisy parallel test runs.
- Fixtures live in `test/fixtures/` and are copied into temporary directories during tests.

## Lint / Formatting / Static Checks
- No dedicated formatter is configured.
- No standalone linter is configured.
- Compiler warnings come from `CMakeLists.txt`: `-Wall -Wextra`.
- Keep formatting consistent with surrounding code instead of reformatting whole files.

## Cursor / Copilot Rules
- No `.cursor/rules/` directory found.
- No `.cursorrules` file found.
- No `.github/copilot-instructions.md` file found.

## Repository Conventions
### Formatting
- Use 2-space indentation, no tabs, and K&R braces: `if (...) {`.
- Keep lines and control flow straightforward rather than clever.
- Prefer small, explicit steps over compressed expressions.

### Includes
- Use project headers with double quotes, for example `"net.h"`; use system headers with angle brackets.
- Keep include blocks tidy and stable.
- Guard platform-specific includes and logic with `_WIN32`.

### Naming
- Files, functions, and variables use `lower_snake_case`.
- Reuse existing enum/type naming patterns such as `Mode`, `Opt`, and `parse_result_t`.

### Types
- Prefer fixed-width integer types for protocol and on-wire data.
- Use `uint16_t` for file name length and port values, `uint64_t` for file sizes and wire-size accounting.
- Use `size_t` for buffer lengths/offsets and `ssize_t` for I/O results.
- Keep socket types portable by using definitions from `src/net.h`.

### Error handling
- Most command-style functions return `0` on success and `1` on failure.
- For invalid CLI usage or validation errors, print a human-readable message to `stderr`.
- Use `perror(...)` only when `errno` is meaningful.
- Use `sock_perror(...)` for socket-related failures.
- Retry interrupted socket operations on `EINTR` or `WSAEINTR`.
- When adding new error paths, verify cleanup still runs exactly once.

### Resource management
- Release every acquired resource on every exit path.
- Use `socket_close(...)` for sockets and `hf_close(...)` for file descriptors.
- Free heap allocations on all paths.
- Prefer one cleanup section per function when practical.
- In server code, separate connection-level cleanup from listener-level cleanup unless you split logic into helper functions.

### Portability
- On Windows, open payload files with `O_BINARY`.
- Preserve existing `_WIN32` branches rather than collapsing them unless the replacement is clearly portable.
- Be careful with APIs whose error reporting differs across POSIX and Windows.

## Protocol Notes
Current frame layout for one transferred file:
1. `file_name_len`: 2-byte big-endian `uint16_t`
2. `file_name`: raw bytes, no trailing NUL on the wire
3. `file_content_size`: 8-byte big-endian `uint64_t`
4. `file_content`: exactly `file_content_size` bytes

Validation expectations:
- Reject `file_name_len == 0` and `file_name_len > 255`.
- Reject unsafe names containing `/`, `\\`, or `..`.

Useful helpers: `protocol_send_header(...)`, `protocol_recv_header(...)`, `send_all(...)`, `recv_all(...)`, `write_all(...)`, `encode_u64_be(...)`, `decode_u64_be(...)`

## Testing and Change Strategy
- Prefer small diffs that preserve the current protocol and CLI behavior.
- For client/server changes, run at least one CLI test and one transfer test.
- For cleanup refactors, re-check every failure path after moving labels or resources.
- Do not assume an advertised CLI flag is implemented; verify in code paths and tests.

## Debugging Notes
- Debug builds define `DEBUG` in `CMakeLists.txt`.
- `DBG(...)` in `src/helper.h` writes to `stderr` when debug is enabled.
- The server readiness log line is `listening on ...`; tests depend on that exact phrase.

## Quick Checklist
- Build: `./build.sh`
- Rebuild: `cmake --build build`
- Single transfer test: `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Single CLI test: `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`
- Full suite: `python3 -m unittest discover -s test -p 'test_*.py' -v`
