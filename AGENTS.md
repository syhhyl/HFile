# AGENTS

This file orients agentic coding assistants working in this repo.
It documents how to build/test and the local code conventions.
If this file conflicts with the codebase, prefer the codebase.

## Project Overview

- Language: C (CMake + Ninja)
- Binary: `hf`
- Modules: `src/main.c`, `src/server.c`, `src/client.c`, `src/helper.c`
- Tests: Python `unittest` in `test/test.py`

## Build / Lint / Test

### Build

Prereqs: `cmake` (>= 3.16) and `ninja`.

- Configure + build + install (default Debug; also writes `build/compile_commands.json`):
  - `./build.sh`
- Release build:
  - `BUILD_TYPE=Release ./build.sh`
- Manual CMake (equivalent to `./build.sh`):
  - `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
  - `cmake --build build`
  - `cmake --install build`

Artifacts:
- In-tree binary: `build/hf`
- Installed binary (default): `$HOME/.local/bin/hf`

### Run

CLI (see `src/helper.c` usage):
- Server: `hf -s <output_dir> [-p <port>]`
- Client: `hf -c <file_path> [-i <ip>] [-p <port>]`

Examples:
- `./build/hf -s ./output -p 9001`
- `./build/hf -c ./input/hello.txt -i 127.0.0.1 -p 9001`

Defaults:
- Port: `9000`
- IP (client): `127.0.0.1`

### Tests (Python)

Python requirement: 3.10+ (uses `Path | None` and `tuple[...]` typing).

- All tests (discovery):
  - `python3 -m unittest discover -s test -p 'test.py' -v`
- Single test method (fastest iteration):
  - `python3 -m unittest discover -s test -p 'test.py' -v -k test_empty_file`
- Single test class:
  - `python3 -m unittest discover -s test -p 'test.py' -v -k TestHFile`

Notes:
- Tests start a server subprocess in `setUpClass` and expect a built `hf`.
- Binary resolution: prefers `$HOME/.local/bin/hf`; falls back to `build/hf`.
- Tests bind port `9000`; avoid running parallel servers/tests on the same port.
- `test_large_file` truncates a 2 GiB file; it may be slow or disk/FS dependent.

### Lint / Format

- No formatter/linter is configured.
- Compiler warnings are enabled: `-Wall -Wextra` (see `CMakeLists.txt`).

## Known Gotchas (Current Repo State)

- `./build.sh` currently fails: `src/main.c` references `Opt.exit_code`, but `Opt` in `src/helper.h` has no `exit_code` field.
- There is also at least one warning: `src/helper.c: parse_args` is `int` but does not return a value on all paths.

If you are asked to "make tests pass", expect to fix build errors first.

## Code Style and Conventions (C)

Follow the existing structure in `src/*.c` / `src/*.h`, but prefer the rules below when touching code.

### Formatting

- Indentation: 2 spaces; no tabs.
- Braces: K&R (`if (...) {` on the same line).
- Prefer braces for multi-line blocks; keep one-statement branches on one line only when very short.
- Blank lines: separate logical blocks; avoid large vertical gaps.
- Keep line lengths reasonable; avoid over-wrapping function calls.

### Includes

- Order includes as: standard library headers, then project headers.
- Use `<...>` for standard headers (e.g. `<stdint.h>`, `<errno.h>`), and `"..."` for local (`"helper.h"`).
- Avoid including standard headers via quotes (the current code has a few; fix opportunistically when editing nearby).

### Naming

- Files: `lower_snake_case.c/.h`.
- Functions/variables: `lower_snake_case`.
- Types/enums: existing code uses `Mode`, `Opt`; keep consistent within the file.
- Constants: prefer `const` variables; use `#define` only for true compile-time constants (e.g. buffer sizes).

### Types

- Use fixed-width integers for on-the-wire/protocol fields: `uint16_t` for ports and filename length.
- Use `size_t` for buffer sizes/lengths; `ssize_t` for `read`/`write` return values.
- Cast only when required by an API; keep signed/unsigned conversions explicit.

### Error Handling

- Convention: return `0` on success, `1` on failure.
- Use `perror("...")` for syscall failures where `errno` is meaningful.
- Use `fprintf(stderr, ...)` for validation/usage errors.
- Handle `EINTR` for blocking syscalls (`accept`, `read`, `write`).
- Prefer early returns for simple functions; use `goto cleanup` when multiple resources must be released.

### Resource Management

- Always close file descriptors/sockets on all exit paths.
- Free heap allocations on all exit paths.
- When using `goto`, keep labels at the end and avoid jumping over initializations.

### Networking / Protocol

Current protocol (see `src/client.c`, `src/server.c`):
1) client sends 2-byte filename length (network byte order)
2) client sends filename bytes (no NUL terminator; max 255)
3) client streams raw file bytes until EOF

Server-side validation:
- Reject `file_len == 0` or `file_len > 255`.
- Reject path traversal in filename (`/`, `\\`, `..`).

I/O helpers:
- `write_all` / `read_all` in `src/helper.c` retry on `EINTR` and attempt full transfers.

### Debug / Logging

- Debug macro: `DBG(...)` in `src/helper.h` (enabled via `DEBUG` compile definition in Debug builds).
- Runtime errors: `perror`/`fprintf(stderr, ...)`.
- Avoid noisy stdout logs unless they are part of the CLI UX (server prints connection/status today).

## Repository Structure

- `src/`: C sources/headers
- `test/`: Python tests and `fixtures/`
- `build/`: generated CMake/Ninja output

## Cursor / Copilot Rules

- No `.cursor/rules/`, `.cursorrules`, or `.github/copilot-instructions.md` are present in this repo.

## Quick Checklist For Agents

- Build: `./build.sh` (or fix build errors first if it fails).
- Run all tests: `python -m unittest -v`.
- Run one test: `python -m unittest -v test.test.TestHFile.test_empty_file`.
- Keep 2-space indent and K&R braces; use `<...>` for standard headers.
- Use `perror`/`fprintf(stderr, ...)` and clean up resources on all paths.
