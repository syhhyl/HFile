# HFile
HFile is a LAN file transfer tool with CLI and Web UI, enabling quick file and message sharing between devices via QR codes or IP addresses.

## HFile idea
This project was inspired by [chfs](https://github.com/amorphobia/chfs)￼. In addition to implementing HTTP-based file serving, I added native client support and accelerated file transfers using platform-specific system functions.

## Advantages of HFile
- Single-binary deployment with no external dependencies and minimal resource usage; frontend HTML is embedded, reducing runtime file dependencies and distribution complexity
- Supports multi-protocol access (HTTP + custom native protocol) with a unified entry point, covering both browser and CLI client use cases
- On Linux, leverages zero-copy mechanisms such as sendfile and splice to accelerate transfers, reducing user/kernel data copies and context switches
- Streaming transfer with segmented disk writes to avoid loading large files entirely into memory, lowering peak memory usage
- Cross-platform implementation (POSIX / Windows) with consistent core capabilities and interface semantics

## Install
Get the installation script from:
https://syhhyl.github.io/HFile/

## How to use HFile
Run the following command to view available options and usage details:
`hf -h`

## License
Apache License 2.0. See `LICENSE`.
