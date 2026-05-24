# HFile
HFile is a LAN file transfer tool

## Advantages of HFile
- Single-binary deployment with no external dependencies and minimal resource usage
- Uses a custom native protocol for CLI file upload
- On Linux, leverages zero-copy mechanisms such as sendfile and splice to accelerate transfers, reducing user/kernel data copies and context switches
- Streaming transfer with segmented disk writes to avoid loading large files entirely into memory, lowering peak memory usage
- Cross-platform implementation (POSIX / Windows) with consistent core capabilities and interface semantics

## How to use HFile
Run the following command to view available options and usage details:
`hf -h`

## License
Apache License 2.0. See `LICENSE`.
