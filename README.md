HFile

Usage

- Server (save incoming files to a directory):
  - `hf -s <server_path> [-p <port>]`
- Client (send a file to server):
  - `hf -c <file_path> [-i <ip>] [-p <port>]`

Examples

- Listen on port 9001:
  - `hf -s ./output -p 9001`
- Send a file to 127.0.0.1:9001:
  - `hf -c ./input/a -i 127.0.0.1 -p 9001`
