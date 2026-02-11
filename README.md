### HFile

HFile is a minimal TCP file transfer tool written in C.
It provides a server that saves incoming files to a target directory and a client that sends a local file to the server.


#### How to build

`./build.sh`

#### How to run test

`python3 test/test.py`


#### How to use

- Server (save incoming files to a directory):
  - `hf -s <server_path> [-p <port>]`
- Client (send a file to server):
  - `hf -c <file_path> [-i <ip>] [-p <port>]`
