# HFile Agent Notes

## Build And Verify

- Default local build: `./build.sh`. It configures CMake + Ninja and exports `build/compile_commands.json`.
- `./test.sh` does not build first. Run a build before any test suite.
- CI does not use `build.sh`; it runs `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, then `python -m unittest -v test.test_hf` on macOS, Linux, and Windows.
- Focused verification shortcuts:
  - `./test.sh cli|http|transfer|support`
  - `./test.sh -v test.test_transfer.TestTransferCLI.test_common_file`
  - `python3 -m unittest -v test.test_http test.test_transfer`

## Architecture

- `src/hfile.c` is the only executable entrypoint. `src/cli.c` parses args, then dispatches to client, server, or control commands.
- One server port handles both the native protocol and HTTP/Web UI. Connection sniffing and top-level dispatch live in `src/server.c`.
- Native file receive and HTTP upload both go through `src/transfer_io.c`; keep receive-to-temp-file and atomic finalize there.
- `src/http.c` serves the HTTP API and Web UI shell. Static Web UI assets are embedded in `src/webui.c`, so editing UI strings/assets always requires rebuilding the binary.
- `src/message_store.c` is the shared latest-message store for both TCP `-m` and `POST /api/messages`.

## Behavior That Tests Depend On

- File transfer stays two-phase: validate header/prefix, send `READY`, stream body, then send `FINAL`.
- Large uploads are streaming. Do not replace file-body receive paths with whole-body `recv_all` logic.
- Filename validation is intentionally strict across CLI and HTTP paths; update the matching tests if behavior changes.
- `hf -d <path>` is the only server start command. Only one HFile server may run at a time.
- POSIX `-d` daemonizes; Windows keeps running attached but still writes control state so `status`, `stop`, and `-q` work.

## Test Quirks

- `test/support/hf.py` always starts servers with `-d` and waits for exact startup markers: `HFile daemon ready` on POSIX, `HFile server ready` on Windows. If you change startup logging, update the helper too.
- Helper-managed tests assume global single-server state. Parallel test runs will collide unless daemon-state files are isolated.
- The helper captures subprocess output as UTF-8; keep that in mind when changing non-ASCII output such as QR-code or message paths.

## Editing Guidance

- Follow the existing C style: 2-space indent, same-line braces, explicit `#ifdef _WIN32` branches.
- Keep `src/net.c` at the socket/endian/sendfile layer; do not move higher-level receive-to-disk logic there.
- When changing CLI parsing, protocol framing, HTTP behavior, control state, or filesystem rules, update the corresponding unittest module in `test/` and run the smallest relevant suite.
- Minimum verification for non-trivial C changes: `cmake --build build` plus the most relevant `python3 -m unittest ...` target.
