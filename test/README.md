## Test Layout

- `test/test_support.py`: helper-level tests for shared test infrastructure.
- `test/test_cli.py`: CLI validation and runtime smoke tests.
- `test/test_transfer.py`: end-to-end transfer and protocol integration tests.
- `test/test_hf.py`: suite wrapper used by the documented unittest commands.
- `test/support/hf.py`: shared helpers for process management, temp dirs, and file assertions.
- `test/fixtures/transfer/`: checked-in payloads used by transfer tests.

Runtime input and output files are created under temporary directories during test runs.

Common commands:

- `./test.sh`: run the full documented test suite.
- `./test.sh transfer`: run the transfer suite.
- `./test.sh cli`: run the CLI suite.
- `./test.sh -v test.test_transfer`: pass raw `unittest` arguments through.
