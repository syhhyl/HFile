# HFile Agent Notes

## Build

- Default: `./build.sh` (Debug, CMake+Ninja, exports `build/compile_commands.json` for LSP).
- Release: `BUILD_TYPE=Release ./build.sh` or `./build.sh -t Release`.
- CI (Ubuntu/macOS, no Windows): `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure`.

## Test

- `./test.sh` runs all tests but does NOT build first — always build first.
- `./test.sh unit`, `./test.sh integration` for focused runs.
- `ctest --test-dir build -R <name> --output-on-failure` works directly.
- Tests use a minimal C harness (`test/test.h`) with `TEST`/`ASSERT`/`ASSERT_EQ`/`ASSERT_STREQ` macros. No external test dependencies.
- `test/test_unit.c` exercises static functions by `#include`-ing the source `.c` files directly (`#include "../src/net.c"` etc.) — this is how private helpers get tested.
- `test/test_integration.c` fork/execs the `hf` binary. It allocates TCP ports sequentially from 19900 without locking — do not run integration tests in parallel.
- Integration tests resolve the binary via `$HF_PATH` (default `./build/hf`). When run via CTest, `hf_int_test` receives the binary path and project root as argv.
- `test/fixtures/transfer/` contains checked-in payloads used by transfer tests.

## CLI

- Entrypoint: `src/hfile.c` → dispatches to `node_recv()` or `node_send()` in `src/node.c`.
- `hf recv [<dir>] [-p <port>]` — foreground receive loop (stop with SIGTERM/Ctrl-C). Default dir is `.`, default port is **8888**.
- `hf send <file> -i <ip> [-p <port>]` — `-i` is required; discovery is not supported.
- Port validation rejects 0 and >65535.
- Filename validation (`ok_name` in `src/node.c`): rejects empty, `/`, `\`, any name containing `..` (including `a..b`). This is intentionally strict.

## Protocol

- Wire format: `header(13B) + prefix(266B) + body`. Total preamble = **279 bytes**.
- Header: magic(2) + version(1) + msg_type(1) + flags(1) + payload_size(8). `magic=0x0429`, `version=0x03`, `msg_type=0x01`, `flags=0x00`.
- Prefix: name_len(2) + name(256B padded) + file_size(8).
- Two-phase: validate preamble → `READY(4B)` → stream body → `FINAL(4B)`. Response frames: phase(1) + status(1) + error_code(2). Max filename length 255.
- Node sends file body via `sendfile()` (Linux/macOS) with buffered fallback; receives via chunked `recv`+`write`.
- Received files go through temp paths (`<name>.tmp.<pid>.<attempt>`) then `rename()` for atomic finalize. This logic lives in `src/node.c`, NOT in `src/net.c`.

## Architecture Boundaries

| Layer | File | Role |
|-------|------|------|
| CLI entrypoint | `src/hfile.c` | Arg parsing, dispatch |
| Protocol & transfer | `src/node.c` | File recv/send, preamble, reply, streaming, temp-file + rename |
| Socket I/O | `src/net.c` | `send_all`, `recv_all`, `socket_close`, `is_socket_invalid` |

- `src/net.c` must NOT contain temp-file or atomic-finalize logic — those stay in `src/node.c`.

## Style

- 2-space indent, same-line braces, explicit `#ifdef _WIN32` branches.
- `src/node.c` uses `static` helpers (`be64_read`, `be64_write`, `ok_name`, `join_path`, `tmp_path`, `reply`, `send_body`, `recv_body`). These are tested via `test_unit.c`'s `#include` trick only.

## Verification

For any non-trivial C change:
```
cmake --build build && ctest --test-dir build --output-on-failure
```
When changing CLI parsing, protocol framing, or filename rules, update both `test/test_unit.c` and `test/test_integration.c`.
