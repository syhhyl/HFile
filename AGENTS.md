# HFile Agent Notes

## Build And Verify

- Default local build: `./build.sh` (Debug CMake + Ninja, exports `build/compile_commands.json`). Use `BUILD_TYPE=Release ./build.sh` or `./build.sh -t Release` for Release.
- `./test.sh` does NOT build first. Build before running tests.
- CI builds on Ubuntu/macOS with raw CMake: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, then `ctest --test-dir build --output-on-failure`.
- Focused suites: `./test.sh unit`, `./test.sh integration`.

## Architecture

- `src/hfile.c` is the only executable entrypoint. It parses args and dispatches to node send/recv.
- Receive mode is `hf recv [<dir>] [-p <port>]`; send mode is `hf send <file> [-i <ip>] [-p <port>]`.
- TCP `<port>` handles the native protocol in `src/node.c`; UDP discovery opens on `<port> + 1` when possible.
- Native file receive goes through `src/node.c`; keep receive-to-temp-file and atomic finalize out of `net.c`.
- `src/net.c` is the socket/zero-copy layer: `sendfile` for sends on Linux/macOS and `splice` for receives on Linux, with buffered fallback. Do NOT move temp-file or atomic-finalize logic into `net.c`.

## Behavior That Tests Depend On

- File transfer stays two-phase: validate header/prefix, send `READY`, stream body, then send `FINAL`.
- Large uploads are streaming. Do not replace file-body receive paths with whole-body buffered recv logic.
- Filename validation is intentionally strict across CLI paths; update the matching tests if behavior changes.
- `hf recv [<dir>] [-p <port>]` runs a foreground receive node and loops waiting for connections. Stop it manually with the process signal or Ctrl-C.

## Test Infrastructure

- Tests are written in C using a minimal framework (`test/test.h`). No external test dependencies.
- `test/test_unit.c`: white-box unit tests for internal functions (includes `src/*.c` directly to access static functions).
- `test/test_integration.c`: black-box integration tests (spawns `hf` binary, tests CLI, transfer, and raw protocol).
- Integration tests find the `hf` binary via `$HF_PATH` or default to `./build/hf`.
- `test/fixtures/transfer/`: checked-in payloads used by transfer tests.

## Editing Guidance

- Follow existing C style: 2-space indent, same-line braces, explicit `#ifdef _WIN32` branches.
- When changing CLI parsing, protocol framing, or filesystem rules, update `test/test_unit.c` and/or `test/test_integration.c`.
- Minimum verification for non-trivial C changes: `cmake --build build && ctest --test-dir build --output-on-failure`.
