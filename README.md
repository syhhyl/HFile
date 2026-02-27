# HFile

HFile is a minimal TCP file transfer tool written in C.

## How to build

deafault to build (macOS and Linux)
```sh
./build.sh
```

if you want to build for Windows, use MinGW

```sh
./build.sh -w
```

want to build for Release
```sh
./build.sh -t Release
```

Install ?
```sh
./build.sh -i
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

Run all tests:

```sh
python3 test/test.py
```

Run a single test:

```sh
python3 -m unittest -v test.test.TestHFile.test_empty_file
```