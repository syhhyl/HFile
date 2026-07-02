# HFile Agent Notes

## Build

- Default build: `./build.sh` (Debug, CMake + Ninja, writes `build/compile_commands.json`).
- Release build: `BUILD_TYPE=Release ./build.sh` or `./build.sh -t Release`.
- Manual equivalent: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`.

## Test

- Build first; `./test.sh` only runs CTest and does not compile.
- All tests: `cmake --build build && ctest --test-dir build --output-on-failure`.
- Focused suites: `./test.sh unit` or `./test.sh integration`; `cli` and `transfer` are aliases for the full integration test binary.
- There is no per-`TEST` filter in the C harness; the practical smallest granularity is CTest `unit` vs `integration`.
- Unit tests include source files directly (`#include "../src/net.c"`, `#include "../src/node.c"`) to reach `static` helpers.
- Integration tests fork/exec `hf`, allocate localhost TCP ports sequentially from 19900, and are not safe to run in parallel.
- Integration tests use `$HF_PATH` when set; CTest passes the built `hf` path and project root explicitly.

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
- Two-phase transfer: validate preamble -> `READY(4B)` -> stream body -> `FINAL(4B)`. Response frames are phase(1) + status(1) + error_code(2).
- Node sends file body via `sendfile()` (Linux/macOS) with buffered fallback; receives via chunked `recv`+`write`.
- Received files go through temp paths (`<name>.tmp.<pid>.<attempt>`) then `rename()` for atomic finalize. This logic lives in `src/node.c`, NOT in `src/net.c`.

## Boundaries

| Layer | File | Role |
|-------|------|------|
| CLI entrypoint | `src/hfile.c` | Arg parsing, dispatch |
| Protocol & transfer | `src/node.c` | File recv/send, preamble, reply, streaming, temp-file + rename |
| Socket I/O | `src/net.c` | `send_all`, `recv_all`, `socket_close`, `is_socket_invalid` |

- `src/net.c` must NOT contain temp-file or atomic-finalize logic — those stay in `src/node.c`.

## Style

- 2-space indent, same-line braces, explicit `#ifdef _WIN32` branches.
- Keep protocol/transfer helpers in `src/node.c` as `static`; expose them to tests via `test/test_unit.c` includes, not headers.

## Verification

- For non-trivial C changes, run `cmake --build build && ctest --test-dir build --output-on-failure`.
- When changing CLI parsing, protocol framing, or filename rules, update both `test/test_unit.c` and `test/test_integration.c`.
