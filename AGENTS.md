# AGENTS

This file orients agentic coding assistants working in this repo.
It summarizes build/test commands, style, and local conventions.
If this file conflicts with repo behavior, prefer the repo behavior.

## Project Overview

- Language: C (build with CMake/Ninja)
- Binary: `hf`
- Primary modules: `src/main.c`, `src/server.c`, `src/client.c`
- Tests: Python `unittest` in `test/test.py`

## Build, Lint, Test

### Build

- Configure + build (default Debug):
  - `./build.sh`
- Manual CMake (equivalent):
  - `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local`
  - `cmake --build build`
  - `cmake --install build`
- Build type override:
  - `BUILD_TYPE=Release ./build.sh`

### Run

- Server mode:
  - `./build/hf -s ./output -p 9001`
- Client mode:
  - `./build/hf -c ./input/a -i 127.0.0.1 -p 9001`

### Tests (Python)

- All tests (preferred):
  - `python -m unittest -v`
- Explicit test module:
  - `python -m unittest -v test.test`
- Single test case:
  - `python -m unittest -v test.test.TestHFile.test_empty_file`
- Single test class:
  - `python -m unittest -v test.test.TestHFile`

Notes:
- Tests start `hf` as a subprocess and need a built binary.
- The test harness looks for `$HOME/.local/bin/hf` and falls back to `build/hf`.

### Lint / Format

- No lint or formatting tools are configured in this repo.
- Do not introduce new tooling unless requested.

## Code Style and Conventions

Follow existing patterns from `src/*.c` and `src/*.h`.

### Formatting

- Indentation: 2 spaces; no tabs.
- Braces: K&R style on the same line.
- Blank lines: used to separate logical blocks; no excessive vertical space.
- Line length: keep reasonable; avoid wrapping unless necessary.

### Includes

- Order: standard library headers first, then project headers.
- Use `<...>` for standard headers and `"..."` for local headers.
- Keep include lists minimal and file-specific.

### Naming

- Files: lower_snake_case for C files (`server.c`, `client.c`).
- Functions: lower_snake_case (`get_file_name`, `parse_port`).
- Variables: lower_snake_case; avoid single-letter names except for indexes or `n`.
- Constants: use `const` variables; avoid macros unless needed.

### Types

- Use fixed-width types when on-the-wire or protocol-related (`uint16_t`).
- Use `size_t` for sizes/lengths of buffers and arrays.
- Use `ssize_t` for return values of `read`/`recv`.
- Keep casts minimal; cast only when required by APIs.

### Error Handling

- Return `1` on failure, `0` on success (matches current style).
- Use `perror()` for system call failures where `errno` applies.
- Use `fprintf(stderr, ...)` for validation/usage errors.
- Prefer early returns or `goto` cleanup labels for resource management.
- Always close file descriptors/sockets on error paths.

### Resource Management

- Close sockets and files explicitly.
- Free heap allocations (`malloc`) on all exit paths.
- Use `goto` labels for centralized cleanup when multiple resources exist.

### Networking Protocol

- Client sends:
  1) 2-byte filename length (network byte order)
  2) filename bytes
  3) raw file bytes
- Server validates filename length and rejects path traversal (`/`, `\\`, `..`).
- Keep protocol changes backward compatible unless explicitly planned.

### Logging and Debug

- Runtime errors: `perror` and `fprintf(stderr, ...)`.
- Debug macro available in `src/helper.h` (uses `DEBUG` define).
- Avoid noisy stdout logs unless needed for user-facing output.

## Repository Structure

- `src/`: C source and headers
- `test/`: Python tests and fixtures
- `build/`: CMake/Ninja output (generated)

## Testing Guidelines

- Build before running tests.
- Tests spawn a server on port 9000; avoid running parallel servers on same port.
- Tests write to `test/HFileTest_OUT` and may clear that directory.

## Contribution Notes

- Keep changes minimal and localized.
- Match existing style and patterns; do not reformat unrelated code.
- Avoid introducing new dependencies unless requested.

## Cursor / Copilot Rules

- No `.cursor/rules/`, `.cursorrules`, or `.github/copilot-instructions.md` found.

## Quick Checklist for Agents

- Confirm build command: `./build.sh`.
- Run tests via `python -m unittest -v`.
- Single test: `python -m unittest -v test.test.TestHFile.test_empty_file`.
- Follow 2-space indentation and K&R braces.
- Use `perror` + `stderr` for errors; clean up resources on all paths.
