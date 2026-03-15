# AGENTS
Guide for coding agents working in this repository.
If these notes conflict with the code or tests, trust the code and keep diffs small.

## Project Snapshot
- Product code is C; tests are Python `unittest`.
- Build system is CMake with Ninja, usually driven through `./build.sh`.
- Tests run the repo-local binary at `build/hf` or `build/hf.exe`.
- The program sends one file or one text message over one TCP connection.
- Portability matters: keep POSIX and Windows behavior split behind `_WIN32`.
- Compiler warnings are enabled, but there is no dedicated lint target.
## Repository Map
- `src/main.c`: entry point and top-level mode dispatch.
- `src/cli.c`, `src/cli.h`: CLI parsing, usage text, and option structs.
- `src/client.c`, `src/client.h`: client-side file and text send paths.
- `src/server.c`, `src/server.h`: listener, receive loop, and save path.
- `src/protocol.c`, `src/protocol.h`: wire header encoding and payload prefix helpers.
- `src/net.c`, `src/net.h`: socket setup, send/recv helpers, endian helpers, socket errors.
- `src/fs.c`, `src/fs.h`: file I/O, basename/path helpers, temp-file helpers.
- `src/helper.c`, `src/helper.h`: monotonic timing and perf reporting.
- `test/test_support.py`: tests for shared Python harness helpers.
- `test/test_cli.py`: CLI validation and client-side smoke tests.
- `test/test_transfer.py`: end-to-end transfer and protocol integration tests.
- `test/support/hf.py`: subprocess, temp-dir, readiness, and file-assert helpers.
## Agent-Specific Rules
- No `.cursor/rules/` directory exists in this repository.
- No `.cursorrules` file exists in this repository.
- No `.github/copilot-instructions.md` file exists in this repository.
## Build Commands
### Preferred entrypoint
- Debug build: `./build.sh`
- Release build: `BUILD_TYPE=Release ./build.sh`
- Alternate CMake build type: `./build.sh -t RelWithDebInfo`
- Native build and install: `./build.sh --install`
- Windows cross-build: `./build.sh --windows`

### Manual CMake
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
- Build: `cmake --build build`
- Install: `cmake --install build`

### Build notes
- `./build.sh -t ...` feeds `CMAKE_BUILD_TYPE` even though the option name says `target`.
- `./build.sh --install` works even though `./build.sh -h` does not mention it.
- `build.sh` records the last platform in `.build_platform`; switching native vs Windows deletes `build/` and reconfigures.
- Do not reuse one `build/` directory for both native and Windows artifacts.
- Use `cmake --build build` for the fastest rebuild after small edits.
- `CMakeLists.txt` enables `-Wall -Wextra` on GCC/Clang and `/W4` on MSVC.
## Lint And Static Checks
- There is no configured lint, formatter, clang-tidy, or sanitizer target.
- Treat a clean rebuild with `cmake --build build` as the primary warning check.
- Do not invent formatter or lint commands unless the user explicitly asks for them.
## Test Commands
### Full suite
- `python3 -m unittest discover -s test -p 'test_*.py' -v`
- `python3 -m unittest -v test.test_hf`

### Single test file
- `python3 -m unittest -v test.test_support`
- `python3 -m unittest -v test.test_cli`
- `python3 -m unittest -v test.test_transfer`

### Single test class
- `python3 -m unittest -v test.test_support.TestSupport`
- `python3 -m unittest -v test.test_cli.TestCLI`
- `python3 -m unittest -v test.test_transfer.TestTransfer`

### Single test method
- `python3 -m unittest -v test.test_support.TestSupport.test_resolve_hf_path_prefers_native_binary`
- `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`
- `python3 -m unittest -v test.test_cli.TestCLI.test_invalid_cli_args`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_common_file`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_protocol_header_validation`

### Test notes
- Build first; runtime tests expect `build/hf` to exist.
- `test/test_transfer.py` launches a real `hf` server subprocess.
- Server readiness is detected from stdout containing `listening on `.
- Transfer tests reserve a free TCP port dynamically, so avoid noisy parallel test runs.
- Transfer fixtures live under `test/fixtures/transfer/`.
- For CLI-only changes, run at least one CLI test; for client, server, protocol, net, or fs changes, run one CLI test and one transfer test.
## Change Strategy
- Prefer small, local diffs over broad cleanup.
- Preserve current CLI flags and the wire protocol unless the task explicitly changes them.
- Trust the code and tests over README-style assumptions.
- Keep unrelated spacing inconsistencies untouched unless the change requires otherwise.
- Be careful in dirty worktrees; do not revert user changes you did not make.
## C Style
### Formatting
- Use 2-space indentation and no tabs.
- Follow the existing K&R brace style: `if (...) {`.
- Keep functions straightforward, explicit, and mostly procedural.
- Short guard clauses are common; use them when they match surrounding code.
- Match local spacing and line breaks in touched blocks instead of reformatting whole files.
- Keep comments sparse; the codebase uses only a few targeted comments and TODOs.

### Includes and headers
- Use project headers with double quotes, for example `"net.h"`.
- Use system headers with angle brackets.
- Match the surrounding include order; most files list project headers first, then system headers.
- Keep header guards uppercase; most use `HF_*`, while `src/helper.h` currently uses `HELPER_H`.
- Avoid moving includes unless a real dependency change requires it.

### Naming
- Files, functions, and local variables use `lower_snake_case`.
- Shared option typedefs and a few enums use short PascalCase names such as `Opt` and `Mode`.
- Enum and result constants use uppercase identifiers such as `PARSE_ERR` and `PROTOCOL_OK`.
- Test classes use `Test...`; test methods use `test_...`.

### Types and data handling
- Use fixed-width integer types for on-wire fields and byte-counted values.
- Use `uint16_t` for ports and short protocol lengths, and `uint64_t` for payload sizes, timestamps, and perf counters.
- Use `size_t` for in-memory sizes and indexes; use `ssize_t` for read, write, send, and recv results.
- Use `socket_t` and helpers from `src/net.h` for portable socket handling.
- Initialize stack structs with `{0}` when partial field assignment follows.
- Encode and decode on-wire integers with helpers such as `encode_u64_be(...)` and `decode_u64_be(...)`.

### Error handling
- Top-level command-style functions return `0` for success and `1` for failure.
- Lower-level helpers may return enums or `0`/`1`; follow the convention already used by that helper family.
- Print concise diagnostics to `stderr`.
- Use `perror(...)` only when `errno` is meaningful, and `sock_perror(...)` for socket-related failures.
- Distinguish EOF from hard I/O errors when receiving protocol data.
- Preserve existing cleanup labels and flow when adding new failure paths.

### Resource management
- Close every file descriptor and socket on every exit path.
- Use `fs_close(...)` for file descriptors and `socket_close(...)` for sockets in shared code.
- Free allocations on both success and failure paths.
- Remove temp files when a transfer fails after temp-file creation.
- A single cleanup label such as `CLEANUP` or `CLEANUP_CONN` is the common pattern when it improves clarity.

### Portability
- Keep Windows-only behavior behind `_WIN32`.
- On Windows, payload files should be opened with `O_BINARY`.
- Do not mix POSIX errno handling with Winsock error APIs.
- Windows socket code may use `SOCKET`, `INVALID_SOCKET`, `SOCKET_ERROR`, and `WSAGetLastError()`.
- Path separators and basename logic differ across platforms; use fs helpers instead of open-coded path parsing.
## Python Test Style
- Keep `from __future__ import annotations` first when present.
- Group standard-library imports before local test-support imports.
- Use `pathlib.Path`, explicit timeouts, and typed helper signatures.
- Prefer `self.assert...` calls with informative failure messages.
- Reuse helpers from `test/support/hf.py` instead of duplicating subprocess or temp-dir logic.
## Protocol And Runtime Notes
- `HF_PROTOCOL_HEADER_SIZE` is `13` bytes; current magic and version are `0x0429` and `0x02`.
- File transfers send file-name length, file name bytes, content size, then content bytes.
- Text messages use `HF_MSG_TYPE_TEXT_MESSAGE` and are capped at `256 KiB`.
- Reject unsafe file names containing `/`, `\\`, or `..`.
- The server ack byte is `0` for success and nonzero for failure.
- Useful helpers: `send_header(...)`, `recv_header(...)`, `proto_send_file_transfer_prefix(...)`, `proto_recv_file_transfer_prefix(...)`, `fs_validate_file_name(...)`, `fs_join_path(...)`.
- Perf reporting comes from `report_transfer_perf(...)` and is written to `stderr`.
## Quick Checklist
- Build first: `./build.sh`
- Rebuild after edits: `cmake --build build`
- Fast support test: `python3 -m unittest -v test.test_support.TestSupport.test_resolve_hf_path_prefers_native_binary`
- Fast CLI check: `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`
- Fast transfer check: `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Full regression: `python3 -m unittest discover -s test -p 'test_*.py' -v`
