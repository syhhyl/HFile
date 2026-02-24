# HFile

HFile is a minimal TCP file transfer tool written in C.

- `hf -s ...` runs a server that receives a file and saves it into a target directory
- `hf -c ...` runs a client that sends a local file to the server

Security note: HFile provides no authentication and no encryption (plaintext). Use only on trusted networks, or protect it with an SSH tunnel / VPN.


## How to build

Requirements: CMake >= 3.16, Ninja, and a C compiler.

Debug build + install (installs to `$HOME/.local/bin/hf`):

```sh
./build.sh
```

Windows cross-compile (MinGW):

```sh
./build.sh -w
```


## CLI Usage

Server:

```sh
hf -s <output_dir> [-p <port>]
```

Client:

```sh
hf -c <file_path> [-i <ip>] [-p <port>]
```

Help:

```sh
hf -h
```


## Tests

The test suite starts a server subprocess and expects a built `hf` binary. It prefers `$HOME/.local/bin/hf` and falls back to `build/hf`.

Run all tests:

```sh
python3 -m unittest discover -s test -p 'test.py' -v
```

Run a single test:

```sh
python3 -m unittest -v test.test.TestHFile.test_empty_file
```