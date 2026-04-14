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

Published release targets:

- macOS arm64
- Linux amd64
- Windows amd64

Build locally:

Requirements:

- CMake 3.16+
- Ninja
- a C compiler
- Python 3 for tests

```bash
./build.sh
```

Release build:

```bash
BUILD_TYPE=Release ./build.sh
```

Install on non-Windows platforms:

```bash
./build.sh -i
```

Cross-build for Windows with MinGW:

```bash
./build.sh -w
```

Output binary:

- native: `build/hf`
- Windows cross-build: `build/hf.exe`

## Usage

CLI forms:

```text
hf -d <server_path> [-p <port>]
hf -c <file_path> [-i <ip>] [-p <port>]
hf -m <message> [-i <ip>] [-p <port>]
hf status
hf stop
hf -q
```

Defaults:

- server/client port: `8888`
- client IP: `127.0.0.1`

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

## HTTP API

List files:

```bash
curl http://127.0.0.1:8888/api/files
```

Upload a file:

```bash
curl -X PUT \
  -H 'Content-Type: application/octet-stream' \
  --data-binary @hello.txt \
  http://127.0.0.1:8888/api/files/hello.txt
```

Download a file:

```bash
curl http://127.0.0.1:8888/api/files/hello.txt -o hello.txt
```

Delete a file:

```bash
curl -X DELETE http://127.0.0.1:8888/api/files/hello.txt
```

Read the latest message:

```bash
curl http://127.0.0.1:8888/api/messages/latest
```


## Test

Build first, then run tests:

```bash
cmake --build build
./test.sh
```


## License

Apache License 2.0. See `LICENSE`.
