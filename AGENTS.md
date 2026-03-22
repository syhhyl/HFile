# Repository Guidelines

## First Principles

HFile is a lightweight, terminal-based file transfer tool built in C around a **minimal, explicit binary protocol**. The architecture follows these core principles:

- **Explicit over implicit**: Protocol messages are self-describing with headers containing magic bytes, version, type, flags, and payload size
- **Minimal dependencies**: Only external dependency is LZ4 for compression; no external libraries for networking or portability
- **Cross-platform native**: Platform-specific code is isolated with `#ifdef _WIN32`; the same codebase runs on Windows and Unix
- **Fail fast**: Parse errors, invalid input, and protocol violations are detected early and reported explicitly
- **Test-driven reliability**: Every behavioral change requires Python `unittest` coverage; integration tests verify end-to-end protocol correctness

The protocol operates over TCP. Clients send messages (file transfers or text) with typed headers; servers parse and respond. Compression is optional per-message via LZ4 blocks.

## Project Structure

```
src/                    C source and headers for the `hf` binary
  hfile.c               Main entry point, mode dispatch
  cli.c                 Argument parsing, usage help, Windows UTF-8 argv handling
  net.c                 Socket I/O helpers (send_all, recv_all), big-endian encoding
  protocol.c            Message encoding/decoding, protocol header operations
  http.c                Embedded HTTP server for web UI
  webui.c               Web UI assets and handlers
  server.c              Server mode: accept connections, handle transfers
  client.c              Client mode: connect, send files/messages
  message_store.c       In-memory message queue for server
  fs.c                  File system utilities (size, existence checks)
  net.h, protocol.h     Core types and function signatures

external/lz4/           LZ4 compression library (static)
test/
  test_*.py             unittest test suites (cli, http, transfer, hf, support)
  support/hf.py         Test helpers: run_hf, resolve_hf_path, reserve_free_port
  fixtures/transfer/    Reusable test payloads (checked into source control)

build/                  Generated build artifacts (never commit)
```

## Build Commands

Use Ninja-backed CMake builds.

```bash
# Configure and build debug binary
./build.sh

# Build optimized release
BUILD_TYPE=Release ./build.sh

# Cross-compile for Windows with MinGW
./build.sh -w

# Install after successful build (non-Windows only)
./build.sh -i

# CI-style configure (no build)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Compile after configure
cmake --build build
```

## Running Tests

```bash
# Full test suite
./test.sh

# Short suite aliases
./test.sh full      # same as running without args
./test.sh support   # test/support/ helpers
./test.sh cli        # CLI argument handling
./test.sh http       # HTTP server behavior
./test.sh transfer   # protocol and file transfer

# Single test class with raw unittest arguments
python -m unittest -v test.test_transfer

# Single test method
python -m unittest -v test.test_transfer.TestTransfer.test_reject_mismatched_magic

# Specific test file
python -m unittest -v test.test_cli

# All tests with verbose output
python -m unittest -v test.test_hf
```

## Code Style Guidelines

### Formatting

- **Indentation**: 2 spaces (no tabs)
- **Braces**: Opening brace on same line for functions and control flow (`if (...) {`)
- **Line length**: Soft limit 100 chars; truncate or break long lines at ~80 chars for readability
- **No formatter**: This project has no automated formatter; match nearby code style manually

### C Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Functions, variables, filenames | `snake_case` | `send_all`, `parse_args`, `net.c` |
| Macros and constants | `UPPER_CASE` | `HF_PROTOCOL_MAX_FILE_NAME_LEN`, `PROTOCOL_ERR_IO` |
| Types and structs | `snake_case_t` or `PascalCase` | `protocol_header_t`, `parse_result_t` |
| Enums | `UPPER_CASE` for values | `PROTOCOL_OK`, `PROTOCOL_ERR_EOF` |
| Private/helper functions | `snake_case` with `_` prefix if internal | `_internal_helper` |

### Type Conventions

- Use `stdint.h` types: `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `int32_t`, `size_t`, `ssize_t`
- Use `socket_t` (defined in `net.h`) for socket descriptors; abstracts platform differences
- Protocol structs are packed with fixed-width integers for wire compatibility
- Prefer signed `int` for return codes (0=success, non-zero=error)

### Import Organization

Order includes in each source file:
1. Corresponding header (e.g., `"protocol.h"` in `protocol.c`)
2. Project headers (e.g., `"net.h"`)
3. Standard library headers (`<stdio.h>`, `<stdlib.h>`, `<string.h>`, etc.)
4. Platform headers last (`#ifdef _WIN32` blocks)

### Error Handling

- **Return codes**: Functions return `0` for success, non-zero for error (POSIX convention)
- **Protocol errors**: Use `protocol_result_t` enum (`PROTOCOL_OK`, `PROTOCOL_ERR_*`)
- **Parse errors**: Return `1` from `parse_args`; use `PARSE_ERR`, `PARSE_HELP` enum
- **I/O errors**: Return `-1` from socket functions; `PROTOCOL_ERR_IO` for protocol layer
- **No exceptions**: C code uses return codes, not exceptions
- **Error propagation**: Check return values at call sites; don't silently ignore errors

### Memory Management

- Use `malloc`/`calloc` for allocation; always check for `NULL`
- Free memory on all exit paths (success and error)
- Use `free(NULL)` safely (it's a no-op)
- Prefer stack allocation for small fixed-size buffers (e.g., `uint8_t buf[8]`)
- Protocol: allocate receive buffers with `+1` for null terminator when treating as string

### Socket I/O Patterns

- Use `send_all`/`recv_all` for guaranteed complete I/O (handles partial reads/writes)
- Always check return values: `-1` = error, `0` = peer closed, positive = bytes transferred
- Protocol recv: check for `PROTOCOL_ERR_EOF` when peer closes connection mid-message
- Windows sockets require `WSAStartup`/`WSACleanup` (see `net_init`/`net_cleanup`)

### Platform-Specific Code

```c
#ifdef _WIN32
    // Windows: SOCKET type, WSAStartup, closesocket()
    SOCKET sock = socket(...);
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    // Unix: int file descriptor, no WSAStartup, close()
    int sock = socket(...);
#endif
```

Keep platform branches explicit and minimal. Centralize platform differences in `net.c` where possible.

### Control Flow

- Use `switch` for argument parsing with single-char flags
- Use `goto` sparingly for error cleanup (rare in this codebase)
- Prefer early returns to reduce nesting
- No `while (1)` loops without `break`/`return` exit condition

## Testing Guidelines

### Python unittest Conventions

- All test files use `from __future__ import annotations` for forward compatibility
- Group tests by feature or regression; name test methods `test_<descriptive_name>`
- Test classes inherit from `unittest.TestCase`
- Use `setUpClass` to resolve the `hf` binary path once per class
- Assert on exact return codes, stderr contains, and protocol behavior

### Test Fixtures

- Store reusable payloads in `test/fixtures/transfer/` (checked into source)
- Create temporary files/directories with `tempfile` module in tests
- Use `make_temp_dir` from `test/support/hf.py` for isolated test directories

### Test Support Helpers (`test/support/hf.py`)

| Function | Purpose |
|----------|---------|
| `resolve_hf_path()` | Find compiled `hf` binary in `build/` |
| `run_hf(path, args, timeout)` | Run `hf` as subprocess, return result |
| `reserve_free_port()` | Get available TCP port for server tests |
| `sha256_file(path)` | Compute file checksum for verification |

### When to Extend Which Test File

| Change Type | Test File |
|------------|-----------|
| CLI argument handling | `test/test_cli.py` |
| Protocol encoding/decoding | `test/test_transfer.py` |
| File transfer logic | `test/test_transfer.py` |
| HTTP server behavior | `test/test_http.py` |
| Shared utilities | `test/test_support.py` |

## Debugging

### Build with Debug Symbols

```bash
./build.sh  # Debug build is default
```

### Verbose Test Output

```bash
python -m unittest -v test.test_transfer.TestTransfer.test_name
```

### Protocol Inspection

For low-level protocol debugging, add temporary `fprintf(stderr, ...)` in `protocol.c` or `net.c`. The protocol header dumps the magic, version, type, flags, and payload size on parse errors.

### Common Issues

- **Connection refused**: Ensure server is running with `hf -s <path>`
- **Port already in use**: Use `reserve_free_port()` in tests to get available port
- **Build fails on Windows**: Ensure MinGW is installed for `./build.sh -w`
- **Tests skip with "hf binary not found"**: Run `./build.sh` first to compile

## Commit & Pull Request Guidelines

- Use **Conventional Commit** style: `type(scope): summary` (e.g., `fix: handle zero-length filenames`)
- Types: `fix`, `feat`, `refactor`, `test`, `docs`, `chore`
- Scope: module name when helpful (e.g., `fix(server):`, `refactor(protocol):`)
- Keep commits focused; one logical change per commit
- Write concise PR descriptions: explain user-visible or protocol impact
- Link related issues in PR body
- Include terminal output/screenshots when CLI behavior changes
- Run `./test.sh` (or relevant subset) before requesting review
- Do not force push to shared branches
