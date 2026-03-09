# AGENTS
Guide for agentic coding assistants working in this repository.
If these notes conflict with the code or tests, trust the code and keep diffs minimal.

## Project Snapshot
- Language: C for product code, Python `unittest` for tests.
- Purpose: transfer one file over one TCP connection.
- Portability target: POSIX and Windows guarded by `_WIN32`.
- Build system: CMake + Ninja, wrapped by `./build.sh`.
- Main binary: `build/hf` on native builds, `hf.exe` when cross-building for Windows.

## Important Paths
- Entry point: `src/main.c`
- CLI parsing: `src/cli.c`, `src/cli.h`
- Client/server: `src/client.c`, `src/client.h`, `src/server.c`, `src/server.h`
- Protocol framing: `src/protocol.c`, `src/protocol.h`
- Platform/socket helpers: `src/net.c`, `src/net.h`
- File I/O wrappers: `src/fs.c`, `src/fs.h`
- Save/path helpers: `src/save.c`, `src/save.h`
- Shared helpers and perf output: `src/helper.c`, `src/helper.h`
- Test helpers: `test/util_hf.py`
- Main test modules: `test/test_cli.py`, `test/test_transfer.py`, `test/test_hf.py`

## Build Commands
### Preferred entrypoint
- Debug build: `./build.sh`
- Release build: `BUILD_TYPE=Release ./build.sh`
- Custom build type: `./build.sh --build-type RelWithDebInfo`
- Build and install natively: `./build.sh --install`

### Manual native build
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
- Build: `cmake --build build`
- Install: `cmake --install build`

### Windows cross-build
- `./build.sh -w`
- `./build.sh --windows`

### Build behavior notes
- `build.sh` stores the active target platform in `.build_platform`.
- Switching between native and Windows builds deletes `build/` and reconfigures.
- Do not run native and Windows builds at the same time against the same `build/` directory.
- Compiler warnings currently come from `CMakeLists.txt`: `-Wall -Wextra`.
- No formatter, linter, sanitizer, or clang-tidy target is configured.

## Test Commands
### Full suite
- `python3 -m unittest discover -s test -p 'test_*.py' -v`
- `python3 -m unittest -v test.test_hf`

### Run one test file
- `python3 -m unittest -v test.test_cli`
- `python3 -m unittest -v test.test_transfer`

### Run one test class
- `python3 -m unittest -v test.test_cli.TestCLI`
- `python3 -m unittest -v test.test_transfer.TestTransfer`

### Run one test method
- `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`
- `python3 -m unittest -v test.test_cli.TestCLI.test_invalid_cli_args`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_common_file`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_fixtures`

### Test behavior notes
- Tests expect `build/hf` to exist first; build before running tests.
- Transfer tests launch a real `hf` server subprocess.
- Server readiness is detected from stdout containing `listening on `.
- Transfer tests reserve a free TCP port dynamically; avoid noisy parallel test runs.
- Fixtures live in `test/fixtures/` and are copied into temporary directories.
- `test/test_hf.py` is a suite wrapper around the CLI and transfer modules.

## Cursor And Copilot Rules
- No `.cursor/rules/` directory exists in this repository.
- No `.cursorrules` file exists in this repository.
- No `.github/copilot-instructions.md` file exists in this repository.

## Change Strategy
- Prefer small diffs that preserve the current CLI and wire protocol.
- Do not assume a documented flag is fully implemented; verify code paths and tests first.
- For client/server changes, run at least one CLI test and one transfer test.
- For cleanup refactors, re-check every failure path and each resource release.
- Keep user changes intact if the worktree is dirty; do not revert unrelated edits.

## Code Style
### Formatting
- Use 2-space indentation and no tabs.
- Follow the existing K&R brace style: `if (...) {`.
- Keep functions explicit and straightforward rather than clever or dense.
- Match surrounding formatting instead of reformatting whole files.
- Follow local style in touched blocks; the repo already has a few spacing inconsistencies.

### Includes and headers
- Put project headers in double quotes, for example `"net.h"`.
- Put system headers in angle brackets.
- Header guards are uppercase macros such as `HF_PROTOCOL_H`.
- Keep include blocks stable and avoid unnecessary churn.
- Guard Windows-only includes and declarations with `_WIN32`.

### Naming
- Files, functions, and variables use `lower_snake_case`.
- Struct typedefs and some enums use short PascalCase-style names like `Opt` and `Mode`.
- Enum constants often use uppercase for result codes (`PARSE_OK`, `PROTOCOL_ERR_IO`) and snake case for mode values (`server_mode`, `client_mode`).
- Reuse existing names and patterns instead of introducing a new naming scheme.

### Types and data handling
- Prefer fixed-width integer types for protocol and byte-counted data.
- Use `uint16_t` for ports and on-wire file-name length.
- Use `uint64_t` for file sizes, timestamps, and wire-size accounting.
- Use `size_t` for buffer lengths and indexing.
- Use `ssize_t` for read/write/send/recv results.
- Use `socket_t` or the definitions in `src/net.h` for portable socket code.

### Error handling
- Most top-level command functions return `0` on success and `1` on failure.
- CLI validation errors should print a short human-readable message to `stderr`.
- Use `perror(...)` only when `errno` is meaningful.
- Use `sock_perror(...)` for socket failures.
- Retry interrupted I/O when surrounding code already handles `EINTR` or `WSAEINTR`.
- Preserve cleanup-once behavior when adding new failure paths.

### Resource management
- Free every allocation and close every descriptor on every exit path.
- Use `socket_close(...)` for sockets and `hf_close(...)` for file descriptors.
- Prefer a single cleanup label per function when practical.
- In the server, keep listener cleanup separate from per-connection cleanup unless a helper extraction clearly simplifies things.

### Portability
- Keep `_WIN32` branches intact unless the replacement is clearly portable.
- On Windows, payload files should be opened with `O_BINARY`.
- Be careful with socket error handling because POSIX and Winsock differ.
- Windows code may use `SOCKET`, `INVALID_SOCKET`, `SOCKET_ERROR`, and `WSAGetLastError()`.

## Protocol Notes
Current frame layout for one file transfer:
1. `file_name_len`: 2-byte big-endian `uint16_t`
2. `file_name`: raw bytes with no trailing NUL on the wire
3. `file_content_size`: 8-byte big-endian `uint64_t`
4. `file_content`: exactly `file_content_size` bytes

Validation expectations:
- Reject `file_name_len == 0`.
- Reject `file_name_len > 255`.
- Reject unsafe names containing `/`, `\\`, or `..`.

Useful helpers:
- `protocol_send_header(...)`, `protocol_recv_header(...)`
- `send_all(...)`, `recv_all(...)`, `write_all(...)`
- `encode_u64_be(...)`, `decode_u64_be(...)`
- `save_validate_file_name(...)`, `save_join_path(...)`

## Debugging Notes
- Debug builds define `DEBUG` in `CMakeLists.txt`.
- `DBG(...)` in `src/helper.h` writes to `stderr` only in debug builds.
- The exact readiness log phrase is `listening on ...`; tests depend on that wording.

## Quick Checklist
- Build first: `./build.sh`
- Rebuild after edits: `cmake --build build`
- Fast CLI check: `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`
- Fast transfer check: `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Full regression: `python3 -m unittest discover -s test -p 'test_*.py' -v`
