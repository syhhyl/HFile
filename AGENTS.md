# HFile Agent Notes

## Build And Verify

- Default local build: `./build.sh` (Debug CMake + Ninja, exports `build/compile_commands.json`). Use `BUILD_TYPE=Release ./build.sh` or `./build.sh -t Release` for Release.
- `./test.sh` does NOT build first. Build before running tests.
- CI builds on Ubuntu/macOS/Windows with raw CMake: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, then `python -m unittest -v test.test_hf`.
- Focused suites: `./test.sh cli`, `./test.sh transfer`, `./test.sh support`.
- Single/focused tests pass through to `unittest`, e.g. `./test.sh -v test.test_transfer.TestTransferCLI.test_common_file`.
- Perf shortcuts in `./test.sh` (`perf`, `perf-server`, `perf-client`) run `test/perf_transfer.py`; do not use them as routine verification.

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

## Test Quirks

- `test/support/hf.py` starts foreground nodes as `build/hf recv <out_dir> -p <port>` and waits until the TCP port accepts connections.
- The helper captures subprocess output as UTF-8; keep that in mind when changing non-ASCII output such as paths.
- `test/test_hf.py` is the full-suite entry point referenced by CI; all other suites (`test_cli`, `test_transfer`, etc.) can be run independently.

## Editing Guidance

- Follow existing C style: 2-space indent, same-line braces, explicit `#ifdef _WIN32` branches.
- When changing CLI parsing, protocol framing, or filesystem rules, update the corresponding unittest module and run the smallest relevant suite.
- Minimum verification for non-trivial C changes: `cmake --build build` plus the most relevant `python3 -m unittest ...` target.
