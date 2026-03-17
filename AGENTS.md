# AGENTS
Guide for coding agents working in this repository.
If these notes conflict with the code or tests, trust the code and keep diffs small.

## Project Snapshot
- Product code is C; tests are Python `unittest`.
- The executable target is `hf`.
- The entry point is `src/hfile.c`, not `src/main.c`.
- The app sends one file or one text message over one TCP connection.
- Portability matters: keep POSIX and Windows behavior split behind `_WIN32`.
- Compiler warnings are enabled, but there is no dedicated lint target.

## Repository Map
- `src/hfile.c`: entry point, Windows UTF-8 argv handling, top-level mode dispatch.
- `src/cli.c`, `src/cli.h`: CLI parsing, usage text, and option structs.
- `src/client.c`, `src/client.h`: client-side file and text send paths.
- `src/server.c`, `src/server.h`: listener, receive loop, ack flow, and save path.
- `src/protocol.c`, `src/protocol.h`: wire header encoding and file-prefix helpers.
- `src/net.c`, `src/net.h`: socket setup, send/recv helpers, endian helpers, socket errors.
- `src/fs.c`, `src/fs.h`: file I/O, basename/path helpers, and temp-file helpers.
- `src/helper.c`, `src/helper.h`: monotonic timing and perf reporting.
- `test/test_support.py`: tests for shared Python harness helpers.
- `test/test_cli.py`: CLI validation and client-side smoke tests.
- `test/test_transfer.py`: end-to-end transfer and raw protocol integration tests.
- `test/support/hf.py`: subprocess, protocol-define, temp-dir, and server helpers.
- `test/test_hf.py`: aggregate unittest entrypoint used by CI.

## Agent-Specific Rule Files
- No `.cursor/rules/` directory exists in this repository.
- No `.cursorrules` file exists in this repository.
- No `.github/copilot-instructions.md` file exists in this repository.

## Build Commands
### Preferred entrypoint
- Debug build: `./build.sh`
- Release build: `BUILD_TYPE=Release ./build.sh`
- Alternate CMake build type: `./build.sh -t RelWithDebInfo`
- Native build and install: `./build.sh --install`
- Windows cross-build with MinGW: `./build.sh --windows`
### Manual native CMake
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
- Build: `cmake --build build`
- Install: `cmake --install build`
### Manual Windows cross-build
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc`
- Build: `cmake --build build`
### Build notes
- `./build.sh -t ...` sets `CMAKE_BUILD_TYPE` even though the option name says `target`.
- `./build.sh --install` works even though the help text does not document it.
- `build.sh` records the last platform in `.build_platform`; switching native vs Windows deletes `build/` and reconfigures.
- Use `cmake --build build` for the fastest rebuild after small edits.
- `CMakeLists.txt` enables `-Wall -Wextra` on GCC/Clang and `/W4` on MSVC; Windows links `ws2_32` and `shell32`.
- Native Windows builds in CI use direct CMake; the Bash wrapper is mainly for POSIX shells and MinGW cross-builds.

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
- `python3 -m unittest -v test.test_support.TestSupport.test_protocol_define_reads_protocol_header`
- `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`
- `python3 -m unittest -v test.test_cli.TestCLI.test_invalid_cli_args`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_raw_file_transfer_two_phase_success`
- `python3 -m unittest -v test.test_transfer.TestTransfer.test_protocol_header_validation`
### Test notes
- Build first; runtime tests expect `build/hf` or `build/hf.exe` to exist.
- `test/test_transfer.py` launches a real `hf` server subprocess via `HFileServer`.
- Server readiness is detected from log output containing `listening on `.
- Transfer tests reserve a likely-free TCP port dynamically; avoid noisy parallel runs.
- File-save completion is detected with `wait_for_file_stable(...)`.
- Raw protocol tests use `protocol_define(...)` so constants stay aligned with `src/protocol.h`.
- Reuse helpers in `test/support/hf.py` instead of open-coding subprocess, log, or temp-dir logic.
- For CLI-only changes, run at least one CLI test; for client, server, protocol, net, or fs changes, run at least one CLI test and one transfer test.
- Run the full suite before finishing any non-trivial protocol or runtime change.

## Change Strategy
- Prefer small, local diffs over broad cleanup.
- Preserve current CLI flags and wire semantics unless the task explicitly changes them.
- Trust the code and tests over README-style assumptions.
- Keep unrelated spacing inconsistencies untouched unless the change requires it.
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
- Match the surrounding include order in the file you touch instead of globally reordering headers.
- Keep header guards uppercase; most use `HF_*`, while `src/helper.h` currently uses `HELPER_H`.
### Naming
- Files, functions, and local variables use `lower_snake_case`.
- Shared option structs and a few enums use short PascalCase names such as `Opt` and `Mode`.
- Enum and result constants use uppercase identifiers such as `PARSE_ERR` and `PROTOCOL_OK`.
- Cleanup labels are uppercase, for example `CLEANUP` and `CLEANUP_CONN`.
### Types and data handling
- Use fixed-width integer types for on-wire fields and byte-counted values.
- Use `uint16_t` for ports and short protocol lengths; use `uint64_t` for payload sizes, timestamps, and perf counters.
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
- Flush stdout when user-visible prints are part of runtime coordination or tests.
### Portability
- Keep Windows-only behavior behind `_WIN32`.
- On Windows, payload files should be opened with `O_BINARY`.
- Do not mix POSIX `errno` handling with Winsock error APIs.
- Windows socket code may use `SOCKET`, `INVALID_SOCKET`, `SOCKET_ERROR`, and `WSAGetLastError()`.
- Preserve the UTF-8 argv handling in `src/hfile.c` when touching Windows startup code.
- Path separators and basename logic differ across platforms; use fs helpers instead of open-coded path parsing.

## Python Test Style
- Keep `from __future__ import annotations` first when present.
- Group standard-library imports before local test-support imports.
- Use `pathlib.Path`, explicit timeouts, and typed helper signatures.
- Prefer `self.assert...` calls with informative failure messages.
- Reuse helpers from `test/support/hf.py` instead of duplicating subprocess or socket setup.
- For protocol tests, prefer the existing raw-send helpers over ad hoc socket code.

## Protocol And Runtime Notes
- `HF_PROTOCOL_HEADER_SIZE` is `13` bytes; current magic and version are `0x0429` and `0x02`.
- File transfers send file-name length, file-name bytes, and `content_size`, then wait for a one-byte ready ack before sending body bytes.
- Text messages use `HF_MSG_TYPE_TEXT_MESSAGE` and are capped at `256 KiB`.
- The current server/client flow expects file-transfer `payload_size == prefix_size + content_size`.
- Reject unsafe file names containing `/`, `\\`, or `..`.
- The server ack byte is `0` for success and nonzero for failure; file transfers use one ready ack before the body and one final ack after it.
- Useful helpers: `send_header(...)`, `recv_header(...)`, `proto_send_file_transfer_prefix(...)`, `proto_recv_file_transfer_prefix(...)`, `fs_validate_file_name(...)`, `fs_join_path(...)`.

## Quick Checklist
- Build first: `./build.sh`
- Rebuild after edits: `cmake --build build`
- Fast CLI check: `python3 -m unittest -v test.test_cli.TestCLI.test_help_prints_usage`
- Fast transfer check: `python3 -m unittest -v test.test_transfer.TestTransfer.test_empty_file`
- Full regression: `python3 -m unittest discover -s test -p 'test_*.py' -v`
