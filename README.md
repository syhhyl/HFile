# HFile

HFile is a lightweight LAN file transfer tool written in C. One server process exposes three things on the same port: a native CLI protocol, a small HTTP API, and a browser Web UI.

## Why HFile

- One binary, one port, minimal setup.
- Native C implementation with explicit protocol and I/O control.
- CLI file send, CLI message send, browser upload/download, and HTTP API in one server.
- Streaming upload path with temp-file write and atomic finalize.

## Install

Project site and installer entrypoints are published with GitHub Pages from the `docs/` directory.

Install the latest release on macOS and Linux:

```bash
curl -fsSL https://syhhyl.github.io/HFile/install.sh | bash
```

Install the latest release on Windows with PowerShell:

```powershell
irm https://syhhyl.github.io/HFile/install.ps1 | iex
```

The install script downloads the latest prebuilt release archive from GitHub Releases and verifies it with `checksums.txt`.


Build and install:

```bash
BUILD_TYPE=Release ./build.sh -i
```

Install on non-Windows platforms:

```bash
./build.sh -i
```

Cross-build for Windows with MinGW:

```bash
./build.sh -w
```

## Usage

Start a server:

```bash
mkdir -p received
./build/hf -d ./received
```

On POSIX, `-d` starts a daemon in the background. On Windows, `-d` prints a notice that daemon mode is unavailable, then runs the server attached in the current process. Only one HFile server can run at a time.

Send a file:

```bash
./build/hf -c ./hello.txt -i 127.0.0.1 -p 8888
```

Send a short message:

```bash
./build/hf -m "hello from CLI" -i 127.0.0.1 -p 8888
```

Daemon mode on non-Windows platforms:

```bash
./build/hf -d ./received -p 8888
./build/hf status
./build/hf -q
./build/hf stop
```

Web UI:

```text
http://127.0.0.1:8888/
```

## Test

Build first, then run tests:

```bash
cmake --build build
./test.sh
```

## License

Apache License 2.0. See `LICENSE`.
