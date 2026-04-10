# HFile

HFile is a lightweight LAN file transfer tool written in C. One server process exposes three things on the same port: a native CLI protocol, a small HTTP API, and a browser Web UI.

## Why HFile

- One binary, one port, minimal setup.
- Native C implementation with explicit protocol and I/O control.
- CLI file send, CLI message send, browser upload/download, and HTTP API in one server.
- Streaming upload path with temp-file write and atomic finalize.

## Install

Requirements:

- CMake 3.16+
- Ninja
- a C compiler
- Python 3 for tests

Build locally:

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
hf -s <server_path> [-p <port>]
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

Start a foreground server:

```bash
mkdir -p received
./build/hf -s ./received
```

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

## Protocol Notes

- The same TCP listener accepts both raw HFile protocol traffic and HTTP traffic.
- File transfer is two-phase: client sends header + prefix, server replies with a `READY` `res_frame`, client sends the body, then server replies with a `FINAL` `res_frame`.
- Received files are streamed to a temp file and finalized atomically.
- Filename validation is intentionally strict.

## Test

Build first, then run tests:

```bash
cmake --build build
./test.sh
```

Focused suites:

```bash
./test.sh cli
./test.sh http
./test.sh transfer
./test.sh support
```

Direct unittest examples:

```bash
python3 -m unittest -v test.test_hf
python3 -m unittest -v test.test_transfer
python3 -m unittest -v test.test_http.TestHTTP.test_upload_list_and_download
```

## Repo Notes

- `src/server.c` owns the unified listener and protocol dispatch.
- `src/net.c` is low-level socket/endian/sendfile support.
- `src/transfer_io.c` is the shared receive-to-disk path for protocol upload and HTTP upload.
- `src/webui.c` embeds the browser assets.

## Limitations

- No TLS or authentication.
- The latest message is stored in memory only.
- Daemon mode is not supported on Windows.

## License

Apache License 2.0. See `LICENSE`.
