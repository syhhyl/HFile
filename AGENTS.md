# Repository Guidelines

## Build And Test

- Use `./build.sh` as the normal build entrypoint. It configures CMake with Ninja, exports `compile_commands.json`, and deletes `build/` automatically when switching native vs `-w` Windows cross-builds.
- `./test.sh` does not build first. Build before running tests.
- CI uses native CMake commands, not `build.sh`: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, then `python -m unittest -v test.test_hf` on Ubuntu, macOS, and Windows.
- Useful commands:
  - `./build.sh`
  - `BUILD_TYPE=Release ./build.sh` or `./build.sh -t Release`
  - `./build.sh -w` for MinGW cross-builds on Linux
  - `./build.sh -i` installs only for non-Windows builds
  - `./test.sh`, `./test.sh cli`, `./test.sh http`, `./test.sh transfer`, `./test.sh support`
  - `python3 -m unittest -v test.test_transfer.TestTransferCLI.test_common_file`

## Architecture

- `src/hfile.c` is the executable entrypoint. CLI parsing is in `src/cli.c`; dispatch goes to server, client, or control commands.
- The server listens on one port for both the native protocol and HTTP/Web UI. Connection sniffing and dispatch live in `src/server.c`.
- `src/protocol.c` owns the native wire format. Preserve field widths and the current header/prefix/`res_frame` contract.
- `src/transfer_io.c` is the shared receive-to-temp-file path for both native uploads and HTTP uploads.
- `src/net.c` is socket/endian/sendfile support only; do not move higher-level receive-to-disk logic there.
- Web UI assets are embedded in `src/webui.c`; editing them requires rebuilding the binary.

## Behavior To Preserve

- File transfer is two-phase: validate header/prefix, send `READY`, stream the body, then send `FINAL`.
- The same port serves CLI transfer, HTTP API, and browser UI.
- Large file receives are streaming. Do not replace them with whole-body `recv_all` logic.
- Received files must go through temp-file write then atomic finalize.
- Filename validation is intentionally strict; update tests if behavior changes.
- `hf -d <path>` is the only server start command. `-s` is gone.
- Only one HFile server may run at a time.
- Platform difference for `-d`: POSIX daemonizes; Windows prints a notice and runs attached in the current process, but still writes control state so `status`, `stop`, and `-q` work.

## Test And Helper Quirks

- The full suite wrapper is `test.test_hf`; focused suites are `test.test_cli`, `test.test_http`, `test.test_transfer`, and `test.test_support`.
- `test/support/hf.py` starts servers with `-d` on every platform and is sensitive to startup log text. Keep ready messages stable unless you also update the helper.
- The helper now forces UTF-8 when capturing subprocess output; keep that if you touch QR-code or other non-ASCII output paths.
- Tests assume global single-server state. Do not make helper-managed server tests run in parallel without isolating daemon-state files first.

## Editing Guidance

- Follow the existing C style: 2-space indent, same-line braces, explicit `#ifdef _WIN32` branches.
- Prefer small, explicit control flow over abstraction.
- Keep `send_all`/`recv_all` for fixed-size frames only. Streaming bodies should stay in incremental `recv`/write loops.
- When changing CLI, protocol, HTTP, control-state, or filesystem behavior, update the corresponding unittest file in `test/`.
- Minimum validation for meaningful C changes: `cmake --build build` plus the smallest relevant unittest target.
