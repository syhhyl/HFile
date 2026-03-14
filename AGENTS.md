# AGENTS
Guide for coding agents working in this repository.
If these notes conflict with the code or tests, trust the code and keep diffs small.

## Project Snapshot
- Product code is C.
- Tests are Python `unittest`.
- Build system is CMake with Ninja, usually driven through `./build.sh`.
- Main native binary is `build/hf`.
- The program transfers one file or one text message over one TCP connection.
- Portability matters: POSIX and Windows paths are guarded with `_WIN32`.

## Key Paths
- `src/main.c`: entry point and top-level dispatch.
- `src/cli.c`, `src/cli.h`: CLI parsing and option structs.
- `src/client.c`, `src/client.h`: client send paths.
- `src/server.c`, `src/server.h`: server receive paths.
- `src/protocol.c`, `src/protocol.h`: frame encoding and decoding.
- `src/net.c`, `src/net.h`: socket helpers and endian helpers.
- `src/fs.c`, `src/fs.h`: file and path helpers.
- `src/helper.c`, `src/helper.h`: timing, debug, perf helpers.
- `test/test_cli.py`: CLI behavior tests.
- `test/test_support.py`: shared test-helper tests.
- `test/test_transfer.py`: end-to-end transfer tests.
- `test/test_hf.py`: suite wrapper.
- `test/support/hf.py`: test harness and server process helpers.

## Agent-Specific Rules
- No `.cursor/rules/` directory exists.
- No `.cursorrules` file exists.
- No `.github/copilot-instructions.md` file exists.

## Build Commands
### Preferred build entrypoint
- Debug build: `./build.sh`
- Release build: `BUILD_TYPE=Release ./build.sh`
- Custom build type: `./build.sh --build-type RelWithDebInfo`
- Native build and install: `./build.sh --install`

### Manual CMake build
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
- Build: `cmake --build build`
- Install: `cmake --install build`

### Windows cross-build
- `./build.sh -w`
- `./build.sh --windows`

### Build notes
- `build.sh` stores the active platform in `.build_platform`.
- Switching between native and Windows builds deletes `build/` and reconfigures.
- Do not share one `build/` directory between native and Windows builds.
- Warnings come from `CMakeLists.txt` via `-Wall -Wextra` on GCC/Clang and `/W4` on MSVC.
- There is no configured formatter, linter, sanitizer, or clang-tidy target.
- Treat `cmake --build build` as the fastest rebuild after small edits.

## Test Commands
### Full test runs
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

### Test notes
- Build first; tests expect `build/hf` to exist.
- Transfer tests launch a real `hf` server subprocess.
- Server readiness is detected from stdout containing `listening on `.
- Transfer tests reserve a free TCP port dynamically; avoid noisy parallel test runs.
- Test fixtures live under `test/fixtures/transfer/`.
- `test/support/hf.py` resolves the host-appropriate binary from `build/hf` or `build/hf.exe`, so install output is not used by tests.

## Change Strategy
- Prefer small, local diffs over broad cleanup.
- Preserve the current CLI and wire protocol unless the task explicitly changes them.
- Verify behavior in code and tests before trusting README-style assumptions.
- For CLI changes, run at least one CLI test.
- For client, server, protocol, or fs changes, run one CLI test and one transfer test.
- Be careful in dirty worktrees; do not revert unrelated user changes.

## C Style
### Formatting
- Use 2-space indentation and no tabs.
- Follow the existing K&R brace style: `if (...) {`.
- Keep functions straightforward and explicit.
- Match the surrounding style in touched blocks instead of reformatting whole files.
- The repo has a few spacing inconsistencies; avoid opportunistic cleanup.

### Includes and headers
- Use project headers with double quotes, for example `"net.h"`.
- Use system headers with angle brackets.
- Header guards are uppercase macros such as `HF_FS_H`.
- Keep include blocks stable unless a real dependency change requires movement.
- Keep `_WIN32` branches intact when working on cross-platform code.

### Naming
- Files, functions, and variables use `lower_snake_case`.
- Typedef names for shared option structs and enums may use short PascalCase forms such as `Opt` and `Mode`.
- Enum result constants commonly use uppercase names like `PARSE_OK`.
- Mode values use snake case like `server_mode`, `client_mode`, `init_mode`.
- Reuse existing names before adding new naming patterns.

### Types and data handling
- Use fixed-width integer types for on-wire fields and byte-counted values.
- Use `uint16_t` for ports and short wire lengths.
- Use `uint64_t` for content sizes, timestamps, and perf counters.
- Use `size_t` for in-memory buffer sizes and indexes.
- Use `ssize_t` for read, write, send, and recv return values.
- Use `socket_t` and helpers from `src/net.h` for portable socket handling.
- Initialize stack structs with `{0}` when partial field assignment follows.

### Error handling
- Top-level command-style functions usually return `0` for success and `1` for failure.
- CLI validation failures should print a short message to `stderr`.
- Use `perror(...)` only when `errno` is meaningful.
- Use `sock_perror(...)` for socket-related failures.
- Retry interrupted I/O when surrounding code already handles `EINTR` or `WSAEINTR`.
- Preserve existing cleanup flow when adding new failure paths.

### Resource management
- Close every fd and socket on every exit path.
- Use `fs_close(...)` for file descriptors and `socket_close(...)` for sockets.
- Free every allocation on failure and success paths.
- Prefer a single cleanup label when it keeps the function clearer.
- In server code, keep listener cleanup separate from per-connection cleanup unless a helper clearly simplifies the function.

### Portability
- Keep Windows-only code behind `_WIN32`.
- On Windows, payload files should be opened with `O_BINARY`.
- POSIX and Winsock error APIs differ; do not mix them.
- Windows socket code may use `SOCKET`, `INVALID_SOCKET`, `SOCKET_ERROR`, and `WSAGetLastError()`.

## Protocol Notes
- Current transfer header uses a fixed-size protocol header plus message-type metadata.
- File transfers include file-name length, file name bytes, content size, and content bytes.
- Text messages use the same protocol layer but a different `msg_type`.
- Reject unsafe file names containing `/`, `\\`, or `..`.
- Useful helpers: `protocol_send_header(...)`, `protocol_recv_header(...)`, `send_all(...)`, `recv_all(...)`, `encode_u64_be(...)`, `decode_u64_be(...)`, `fs_validate_file_name(...)`, `fs_join_path(...)`.

## Debugging Notes
- Debug builds define `DEBUG` in `CMakeLists.txt`.
- `DBG(...)` in `src/helper.h` writes to `stderr` only in debug builds.
- The exact readiness log phrase is `listening on ...`; tests depend on it.

## Quick Checklist
- Build first: `./build.sh`
- Rebuild after edits: `cmake --build build`
- Fast CLI check: `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`
- Fast transfer check: `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Full regression: `python3 -m unittest discover -s test -p 'test_*.py' -v`
