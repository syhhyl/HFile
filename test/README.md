## Test Layout

- `test/test.h`: minimal C test harness (TEST/ASSERT macros, zero dependencies).
- `test/test_unit_cli.c`: white-box unit tests for CLI argument parsing.
- `test/test_unit_node.c`: white-box unit tests for protocol and transfer helpers in `node.c`.
- `test/test_unit_net.c`: white-box unit tests for socket I/O helpers in `net.c`.
- `test/test_integration_cli.c`: black-box tests for CLI output and exit status.
- `test/test_integration_transfer.c`: client/server file transfer tests.
- `test/test_integration_protocol.c`: raw protocol validation and transfer tests.
- `test/integration_support.c`: shared process, socket, path, and file helpers for integration tests.
- `test/fixtures/transfer/`: checked-in payloads used by transfer tests.

Each integration executable creates an isolated temporary directory. Server tests
ask the operating system for an available loopback port, retry if startup loses
the port, and wait for that child process to print `HFile node ready` before
connecting. The test runner always stops the receiver after each test, including
assertion failures. CTest applies finite timeouts to integration suites.

Common commands:

- `./test.sh`: run all tests (unit + integration).
- `./test.sh unit`: run unit tests only.
- `./test.sh integration`: run integration tests only.
- `./test.sh cli`: run CLI unit and integration tests.
- `./test.sh transfer`: run transfer integration tests only.
- `./test.sh protocol`: run protocol integration tests only.
- `./test.sh --output-on-failure`: pass raw `ctest` arguments through.
