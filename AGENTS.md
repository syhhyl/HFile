# Repository Guidelines

## Project Structure & Module Organization
`src/` contains the C sources and headers for the `hf` binary. Key areas are networking and protocol handling (`net.*`, `protocol.*`, `http.*`), transfer roles (`server.*`, `client.*`), and CLI/bootstrap code (`cli.*`, `hfile.c`). Third-party code lives in `external/lz4/`. Build artifacts are generated under `build/` and should stay untracked. Tests live in `test/`: `test/support/` holds Python helpers, `test/fixtures/transfer/` stores checked-in payloads, and `test/test_*.py` covers CLI, HTTP, transfer, and shared helpers.

## Build, Test, and Development Commands
Use Ninja-backed CMake builds.

- `./build.sh`: configure and build a local debug build in `build/`.
- `BUILD_TYPE=Release ./build.sh`: build an optimized release binary.
- `./build.sh -w`: cross-compile for Windows with MinGW.
- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`: CI-style configure step.
- `cmake --build build`: compile the `hf` executable.
- `python -m unittest -v test.test_hf`: run the full documented test suite.
- `python -m unittest -v test.test_transfer`: run a focused integration test file.

## Coding Style & Naming Conventions
Follow the existing C style: 2-space indentation, opening braces on the same line for functions and control flow, and compact helper functions where appropriate. Use `snake_case` for functions, variables, and filenames; reserve `UPPER_CASE` for macros and constants. Keep platform-specific branches explicit with `#ifdef _WIN32`. There is no repository formatter configured, so match nearby code and compile with warnings enabled (`-Wall -Wextra`, `/W4` on MSVC).

## Testing Guidelines
Add Python `unittest` coverage for every behavioral change. Name new tests `test_*.py` and group methods by feature or regression. Prefer extending `test/test_transfer.py` for protocol or file movement changes, `test/test_cli.py` for argument handling, and `test/test_http.py` for HTTP behavior. Tests create temporary runtime files; keep reusable payloads in `test/fixtures/transfer/`.

## Commit & Pull Request Guidelines
Recent history favors short Conventional Commit-style subjects such as `fix: test_transfer bug` and `refactor(server): extract connection handlers`. Prefer `type(scope): summary` when a scope helps. Keep PRs focused, explain the user-visible or protocol impact, link related issues, and include terminal output or screenshots when CLI behavior changes. Confirm the relevant `python -m unittest` command before requesting review.
