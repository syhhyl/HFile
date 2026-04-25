# HFile Agent Notes

## Build And Verify

- Default local build: `./build.sh` (CMake + Ninja, exports `build/compile_commands.json`).
- `./test.sh` does NOT build first. Build before running tests.
- CI uses raw CMake: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`, then `python -m unittest -v test.test_hf`.
- Focused test shortcuts:
  - `./test.sh cli|http|transfer|support` — suite shortcuts
  - `./test.sh -v test.test_transfer.TestTransferCLI.test_common_file` — single test
  - `python3 -m unittest -v test.test_http test.test_transfer` — parallel suites

## Architecture

- `src/hfile.c` is the only executable entrypoint. `src/cli.c` parses args, dispatches to client/server/control.
- One server port handles both the native protocol and HTTP/Web UI. Connection sniffing and top-level dispatch live in `src/server.c`.
- Native file receive and HTTP upload both go through `src/transfer_io.c`; keep receive-to-temp-file and atomic finalize there.
- `src/http.c` serves the HTTP API and Web UI shell. Static Web UI assets are embedded in `src/webui.c`; editing UI strings always requires rebuilding.
- `src/message_store.c` is the shared latest-message store for both TCP `-m` and `POST /api/messages`.
- `src/net.c` is the zero-copy/socket layer: `sendfile` (downloads, Linux) and `splice` (uploads, Linux). Cross-platform I/O goes through buffered `fs.c` fallback. Do NOT move receive-to-disk logic into `net.c`.

## Shutdown And Exit Codes

- `shutdown_signal_number()` returns the actual signal number (e.g. `SIGINT`) only when a real signal was caught. It is `0` when shutdown was triggered internally via `shutdown_request()`.
- `shutdown_exit_code()` always returns `130`.
- In `main()`, only override the exit code when `shutdown_signal_number() != 0`. This prevents internal cleanup paths (e.g. `server_run_process` calling `shutdown_request()` on error) from being misreported as signal exits.
- `shutdown_request()` sets `g_shutdown_requested = 1` but does NOT set `g_shutdown_signal`. It is used to wake blocking threads (SSE, state watcher, connection tracker) during cleanup.

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
- `test/test_hf.py` is the full-suite entry point referenced by CI; all other suites (`test_cli`, `test_http`, etc.) can be run independently.

## Editing Guidance

- Follow existing C style: 2-space indent, same-line braces, explicit `#ifdef _WIN32` branches.
- When changing CLI parsing, protocol framing, HTTP behavior, control state, or filesystem rules, update the corresponding unittest module and run the smallest relevant suite.
- Minimum verification for non-trivial C changes: `cmake --build build` plus the most relevant `python3 -m unittest ...` target.
