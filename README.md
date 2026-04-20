# HFile

HFile is a native C file transfer tool built for simple deployment, predictable behavior, and low overhead.

## Quick Start

Install the latest release on macOS or Linux:

```bash
curl -fsSL https://syhhyl.github.io/HFile/install.sh | bash
```

Install on Windows:

```powershell
irm https://syhhyl.github.io/HFile/install.ps1 | iex
```


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


## License

Apache License 2.0. See `LICENSE`.
