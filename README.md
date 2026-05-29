# HFile

HFile is a small LAN file transfer prototype.

This repository is kept as a compact systems programming sample rather than an
actively developed product. It demonstrates a simple TCP file transfer protocol,
streaming disk writes, atomic receive-side finalize with a temp file and
`rename()`, and `sendfile()` on supported platforms.

## Usage

```sh
hf -h
```

## Status

Development is stopped. The implementation is useful as a reference or learning
project, but it is not intended to become a general-purpose file transfer tool.

## License

Apache License 2.0. See `LICENSE`.
