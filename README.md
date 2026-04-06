# HFile

HFile is a lightweight native C file-transfer tool for local networks. It combines a custom TCP client/server protocol, a built-in HTTP API, and a small Web UI in a single server process.

## Features

- Send files from the CLI to a receiving server over a custom binary protocol.
- Send short text messages from the CLI to the same server.
- Browse, upload, download, and delete files from the browser.
- View the latest message from the Web UI or HTTP API.
- Run in foreground server mode or daemon mode on non-Windows platforms.
- Print server status, stop the daemon, and show a QR code for mobile access.
- Keep the implementation small and explicit, with minimal dependencies.

## Why HFile

- One binary, one server process, one port.
- Native C implementation with direct control over protocol and I/O.
- Built for practical LAN file sharing without a heavy stack.
- Includes both CLI and browser workflows.

## Requirements

- CMake 3.16+
- Ninja
- A C compiler
- Python 3 for the test suite

Platform notes:

- On non-Windows platforms, the build links against pthreads via CMake.
- Windows cross-builds use `x86_64-w64-mingw32-gcc`.
- Daemon mode is not supported on Windows.

## Building

Preferred build commands:

```bash
./build.sh
BUILD_TYPE=Release ./build.sh
./build.sh -t Release
./build.sh -w
./build.sh -i
```

Direct CMake commands:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cmake --build build --target hf
```

Build output:

- Native build: `build/hf`
- Windows cross-build: `build/hf.exe`

## CLI Usage

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

- Server/client port: `8888`
- Client IP: `127.0.0.1`

## Quick Start

Start a server in the foreground:

```bash
mkdir -p received
./build/hf -s ./received
```

Send a file from another terminal:

```bash
./build/hf -c ./hello.txt -i 127.0.0.1 -p 8888
```

Send a message:

```bash
./build/hf -m "hello from CLI" -i 127.0.0.1 -p 8888
```

Open the Web UI:

```text
http://127.0.0.1:8888/
```

## Running HFile

Foreground server:

```bash
./build/hf -s ./received -p 8888
```

Daemon mode on non-Windows platforms:

```bash
./build/hf -d ./received -p 9000
./build/hf status
./build/hf -q
./build/hf stop
```

`status` reports the current daemon state, receive directory, port, Web UI URL, and related runtime metadata.

## Web UI

After the server starts, open the root URL in a browser:

```text
http://127.0.0.1:8888/
```

The built-in UI supports:

- Uploading files
- Listing files
- Downloading files
- Deleting files
- Posting a message
- Viewing the latest message
- Receiving live updates via server-sent events

## HTTP API

The same server port also serves a small HTTP API.

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

Post a message:

```bash
curl -X POST \
  -H 'Content-Type: application/json' \
  -d '{"message":"hello from browser"}' \
  http://127.0.0.1:8888/api/messages
```

Fetch the latest message:

```bash
curl http://127.0.0.1:8888/api/messages/latest
```

Notes:

- HFile implements a conservative HTTP/1.1 subset.
- Message submission expects `application/json`.
- File upload expects `application/octet-stream`.

## Protocol Notes

- The same TCP listener accepts both raw HFile protocol traffic and HTTP traffic.
- Connection type is detected from the first bytes received.
- The custom protocol uses an explicit header with a fixed magic value and protocol version.
- File transfer uses a ready ACK before the body is sent and a final ACK after completion.
- Received files are written through temporary files and finalized atomically.
- Filename validation is intentionally strict and rejects path traversal-style input.

## Testing

Build before running tests.

Run the full suite:

```bash
./test.sh
./test.sh full
python3 -m unittest -v test.test_hf
```

Run focused suites:

```bash
./test.sh support
./test.sh cli
./test.sh http
./test.sh transfer
```

Run targeted tests directly:

```bash
python3 -m unittest -v test.test_http
python3 -m unittest -v test.test_transfer
python3 -m unittest -v test.test_transfer.TestTransferCLI
python3 -m unittest -v test.test_http.TestHTTP.test_upload_list_and_download
```

Pass raw `unittest` arguments through the helper script:

```bash
./test.sh -v test.test_transfer
./test.sh -v test.test_transfer.TestTransferCLI.test_common_file
```

## Repository Layout

```text
src/
  hfile.c               Program entry point
  cli.c/.h              CLI parsing and help text
  client.c/.h           CLI client send logic
  server.c/.h           Listener and protocol dispatch
  http.c/.h             HTTP parsing and Web UI/API handling
  webui.c/.h            Embedded HTML/CSS/JS assets
  protocol.c/.h         Binary protocol encode/decode
  transfer_io.c/.h      Receive-to-disk helpers
  fs.c/.h               File system helpers and validation
  net.c/.h              Socket helpers and endian helpers
  message_store.c/.h    In-memory latest-message store
  control.c/.h          Status and stop control paths
  daemon_state.c/.h     Daemon runtime state files
  mobile_ui.c/.h        URL and QR display helpers
  shutdown.c/.h         Shutdown handling

third_party/
  qrcodegen.c/.h        Vendored QR code generator

test/
  test_cli.py           CLI validation and smoke tests
  test_http.py          HTTP behavior
  test_transfer.py      Protocol and transfer behavior
  test_support.py       Test helper behavior
  test_hf.py            Full/default suite wrapper
  support/hf.py         Shared test helpers
```

## Platform Support

- Native builds support POSIX platforms.
- Cross-build support exists for Windows.
- Network and filesystem code use explicit platform branches.
- Windows supports the client/server code paths, but daemon mode is rejected.

## Limitations

- No authentication or TLS is built in.
- The latest message is kept in memory rather than stored persistently.
- The README documents behavior confirmed by source and tests; it intentionally avoids unstated guarantees.

## License

Apache License 2.0. See `LICENSE`.
