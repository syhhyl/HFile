# Repository Guidelines

## What Matters

- `./build.sh` is the default build entrypoint. It configures CMake with Ninja, exports `compile_commands.json`, and deletes `build/` automatically when switching native vs `-w` Windows cross-builds.
- `./test.sh` does not build first. Build before running tests.
- CI runs `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, then `python -m unittest -v test.test_hf` on Ubuntu, macOS, and Windows.

## Fast Commands

- Build debug: `./build.sh`
- Build release: `BUILD_TYPE=Release ./build.sh` or `./build.sh -t Release`
- Cross-build Windows: `./build.sh -w`
- Install non-Windows build: `./build.sh -i`
- Full tests: `./test.sh` or `python3 -m unittest -v test.test_hf`
- Focused tests: `./test.sh cli`, `./test.sh http`, `./test.sh transfer`, `./test.sh support`
- Single test: `python3 -m unittest -v test.test_transfer.TestTransferCLI.test_common_file`

## Code Boundaries

- `src/hfile.c` is the entrypoint. CLI parsing is in `src/cli.c`; it dispatches to server, client, or control commands.
- One listener handles both the custom protocol and HTTP. Connection type detection and protocol dispatch live in `src/server.c`.
- `src/net.c` is low-level socket/endian/sendfile support. Do not move upload-to-disk logic there.
- `src/transfer_io.c` is the shared receive-to-temp-file then finalize path used by both raw protocol upload and HTTP upload.
- `src/protocol.c` owns wire framing. Keep field widths exact and compatible with the current header/prefix/`res_frame` contract.
- Web assets are embedded in `src/webui.c`; rebuild after editing them.

## Verified Behavior To Preserve

- File transfer is two-phase: server validates header/prefix, sends a `READY` `res_frame`, receives the body, then sends a `FINAL` `res_frame`.
- The same server port also serves the HTTP API and Web UI.
- Large file receives are streaming; do not replace them with `recv_all` over the whole body.
- Received files are written to a temp file and finalized atomically.
- Filename validation is intentionally strict; avoid weakening it without updating tests.
- Daemon mode exists only on non-Windows platforms.

## Edit Guidelines

- Follow the local C style already in use: 2-space indent, same-line braces, explicit `#ifdef _WIN32` branches.
- Prefer small changes. This codebase values explicit control flow over abstractions.
- Keep `send_all`/`recv_all` for fixed-size frames only. Streaming bodies should continue to use incremental `recv`/write loops.
- When changing protocol, HTTP, CLI, or filesystem behavior, update the relevant unittest file in `test/`.

## Validation Expectations

- For meaningful C changes, at minimum run `cmake --build build` and the smallest relevant unittest target.
- Typical mappings:
  - CLI/help: `python3 -m unittest -v test.test_cli`
  - Protocol/file transfer: `python3 -m unittest -v test.test_transfer`
  - HTTP/Web UI/API: `python3 -m unittest -v test.test_http`
