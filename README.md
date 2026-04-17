# HFile

Bytes with intent.

HFile is a native C file transfer tool built for simple deployment, predictable behavior, and low overhead.

## Why HFile

- One binary, one port, almost no setup.
- CLI, HTTP API, and Web UI in a single server process.
- Send from terminal, browser, or script without changing servers.
- Stream large files instead of buffering entire payloads in memory.
- Write to a temp file first, then atomically finalize on success.
- Native C implementation with a small, explicit protocol surface.

## Quick Start

Install the latest release on macOS or Linux:

```bash
curl -fsSL https://syhhyl.github.io/HFile/install.sh | bash
```

Install on Windows:

```powershell
irm https://syhhyl.github.io/HFile/install.ps1 | iex
```

The installer downloads the latest GitHub Release and verifies it with `checksums.txt`.

Start a server:

```bash
hf -d ./received -p 8888
```

Send a file:

```bash
hf -c ./hello.txt -i 127.0.0.1 -p 8888
```

Send a message:

```bash
hf -m "hello from CLI" -i 127.0.0.1 -p 8888
```

Open the Web UI:

```text
http://127.0.0.1:8888/
```

On POSIX, `-d` starts the server in the background. On Windows, `-d` keeps the server attached to the current process. Only one HFile server can run at a time.

## What It Does

- Receive files from the HFile CLI.
- Receive uploads from a browser on the same port.
- Expose a small HTTP surface for automation.
- Keep transfer behavior simple and predictable.

## For Developers

Build, test, and development details live in [AGENTS.md](./AGENTS.md).

## License

Apache License 2.0. See `LICENSE`.
