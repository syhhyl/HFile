## Test Layout

- `test/test.h`: minimal C test harness (TEST/ASSERT macros, zero dependencies).
- `test/test_unit.c`: white-box unit tests for internal functions (ok_name, be64, reply, join_path, tmp_path, etc.).
- `test/test_integration.c`: black-box integration tests (CLI parsing, file transfer, raw protocol validation).
- `test/fixtures/transfer/`: checked-in payloads used by transfer tests.

Runtime input and output files are created under temporary directories during test runs.

Common commands:

- `./test.sh`: run all tests (unit + integration).
- `./test.sh unit`: run unit tests only.
- `./test.sh integration`: run integration tests only.
- `./test.sh --output-on-failure`: pass raw `ctest` arguments through.
