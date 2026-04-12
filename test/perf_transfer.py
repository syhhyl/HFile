from __future__ import annotations

import argparse
import atexit
import random
import shutil
import signal
import socket
import statistics
import sys
import tempfile
import time
from datetime import datetime
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from test.support.hf import HFileServer, resolve_hf_path, run_hf


MIB = 1024 * 1024
GIB = 1024 * MIB
DEFAULT_SIZES = [1 * GIB, 2 * GIB, 3 * GIB, 4 * GIB]
DEFAULT_RUNS = 5
FILE_CHUNK_SIZE = 1 * MIB
SCAN_INTERVAL = 0.05
TRANSFER_TIMEOUT_SCALE = 30.0
TRANSFER_TIMEOUT_MIN = 30.0


CURRENT_BATCH_DIR: Path | None = None


@dataclass(frozen=True)
class BatchMetrics:
    label: str
    size_bytes: int
    runs: int
    intervals: list[float]


def human_size(size: int) -> str:
    if size % GIB == 0:
        return f"{size // GIB}GiB"
    if size % MIB == 0:
        return f"{size // MIB}MiB"
    return f"{size}B"


def parse_size(text: str) -> int:
    normalized = text.strip().lower()
    units = {
        "gib": GIB,
        "gb": GIB,
        "mib": MIB,
        "mb": MIB,
        "kib": 1024,
        "kb": 1024,
        "b": 1,
    }
    for suffix, factor in units.items():
        if normalized.endswith(suffix):
            value = normalized[: -len(suffix)].strip()
            return int(float(value) * factor)
    return int(normalized)


def parse_sizes(text: str | None) -> list[int]:
    if text is None or not text.strip():
        return list(DEFAULT_SIZES)
    return [parse_size(part) for part in text.split(",") if part.strip()]


def transfer_timeout_seconds(size: int) -> float:
    return max(TRANSFER_TIMEOUT_MIN, size / MIB / TRANSFER_TIMEOUT_SCALE)


def file_seed(label: str, index: int, random_seed: int) -> int:
    seed = random_seed & 0xFFFFFFFFFFFFFFFF
    for ch in f"{label}:{index}":
        seed ^= ord(ch)
        seed = (seed * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return seed


def write_generated_file(path: Path, size: int, seed: int) -> None:
    print(f"Preparing input file: {path.name} ({human_size(size)})")
    rng = random.Random(seed)
    remaining = size
    with path.open("wb") as f:
        while remaining > 0:
            want = min(FILE_CHUNK_SIZE, remaining)
            chunk = rng.randbytes(want)
            f.write(chunk)
            remaining -= want


def run_wash_step(path: Path) -> None:
    with path.open("rb") as f:
        while f.read(FILE_CHUNK_SIZE):
            pass


def cleanup_current_batch_dir() -> None:
    global CURRENT_BATCH_DIR
    if CURRENT_BATCH_DIR is None:
        return
    shutil.rmtree(CURRENT_BATCH_DIR, ignore_errors=True)
    CURRENT_BATCH_DIR = None


def install_cleanup_handlers() -> tuple[object, object]:
    previous_int = signal.getsignal(signal.SIGINT)
    previous_term = signal.getsignal(signal.SIGTERM)

    def handle_signal(signum: int, _frame: object) -> None:
        cleanup_current_batch_dir()
        signal.signal(signum, signal.SIG_DFL)
        signal.raise_signal(signum)

    atexit.register(cleanup_current_batch_dir)
    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)
    return previous_int, previous_term


def restore_cleanup_handlers(previous_int: object, previous_term: object) -> None:
    signal.signal(signal.SIGINT, previous_int)
    signal.signal(signal.SIGTERM, previous_term)


def recv_line(sock: socket.socket) -> str:
    chunks = bytearray()
    while True:
        data = sock.recv(1)
        if not data:
            raise RuntimeError("control connection closed unexpectedly")
        if data == b"\n":
            return chunks.decode("utf-8")
        chunks.extend(data)


def send_line(sock: socket.socket, text: str) -> None:
    sock.sendall(text.encode("utf-8") + b"\n")


def print_summary(metrics_by_batch: list[BatchMetrics]) -> None:
    print(format_summary(metrics_by_batch), end="")


def format_summary(metrics_by_batch: list[BatchMetrics]) -> str:
    lines = ["", "batch     runs  total(s)  avg(s)    median(s)  best(s)  avg(MiB/s)"]
    for metrics in metrics_by_batch:
        intervals = metrics.intervals
        total = sum(intervals)
        throughput = ((metrics.size_bytes * metrics.runs) / MIB) / total
        lines.append(
            f"{metrics.label:<9} {metrics.runs:<5} {total:<9.3f} "
            f"{statistics.mean(intervals):<9.3f} "
            f"{statistics.median(intervals):<10.3f} "
            f"{min(intervals):<8.3f} {throughput:.2f}"
        )
    lines.append("")
    return "\n".join(lines)


def write_results_file(results_dir: Path, summary_text: str) -> Path:
    results_dir.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = results_dir / f"perf_transfer_{timestamp}.txt"
    path.write_text(summary_text, encoding="utf-8")
    return path


def expect_line(actual: str, expected: str, ctx: str) -> None:
    if actual.strip() != expected:
        raise RuntimeError(f"expected {expected!r} for {ctx}, got {actual!r}")


def parse_batch_start(line: str) -> tuple[str, int]:
    parts = line.strip().split(" ", 2)
    if len(parts) != 3 or parts[0] != "BATCH":
        raise RuntimeError(f"invalid batch control line: {line!r}")
    return parts[1], int(parts[2])


def parse_file_start(line: str) -> tuple[str, int]:
    parts = line.strip().split(" ", 2)
    if len(parts) != 3 or parts[0] != "FILE":
        raise RuntimeError(f"invalid file control line: {line!r}")
    return parts[1], int(parts[2])


def wait_for_completed_file(
    out_dir: Path,
    file_name: str,
    expected_size: int,
    timeout: float,
) -> float:
    target = out_dir / file_name
    start = time.monotonic()
    previous_size: int | None = None
    deadline = start + timeout + 10.0

    while time.monotonic() < deadline:
        try:
            size = target.stat().st_size
        except FileNotFoundError:
            previous_size = None
            time.sleep(SCAN_INTERVAL)
            continue

        if size == expected_size and previous_size == size:
            return time.monotonic() - start

        previous_size = size
        time.sleep(SCAN_INTERVAL)

    raise RuntimeError(
        f"timed out waiting for completed file {file_name!r} ({human_size(expected_size)})"
    )


def handle_server_batch(
    conn: socket.socket,
    out_dir: Path,
    label: str,
    expected_size: int,
    runs: int,
) -> list[float]:
    intervals: list[float] = []
    timeout = transfer_timeout_seconds(expected_size)

    for _ in range(runs):
        file_name, file_size = parse_file_start(recv_line(conn))
        if file_size != expected_size:
            raise RuntimeError(
                f"expected file size {expected_size} for {label}, got {file_size}"
            )

        send_line(conn, "READY")
        elapsed = wait_for_completed_file(out_dir, file_name, expected_size, timeout)
        intervals.append(elapsed)
        (out_dir / file_name).unlink(missing_ok=True)
        send_line(conn, f"DONE {file_name}")

    expect_line(recv_line(conn), "END", f"batch end for {label}")
    send_line(conn, "DONE")
    return intervals


def run_server(args: argparse.Namespace) -> int:
    hf_path = resolve_hf_path()
    sizes = parse_sizes(args.sizes)
    labels = [human_size(size) for size in sizes]
    size_by_label = dict(zip(labels, sizes, strict=True))
    metrics_by_batch: list[BatchMetrics] = []

    with tempfile.TemporaryDirectory(prefix="hf_perf_server_") as tmp_dir:
        out_dir = Path(tmp_dir) / "outputs"
        out_dir.mkdir(parents=True, exist_ok=True)

        server = HFileServer(hf_path=hf_path, out_dir=out_dir, port=args.hf_port)
        server.start(startup_timeout=10.0)

        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.control_host, args.control_port))
        listener.listen(1)

        print("mode       : perf-server")
        print(f"hf_port    : {server.port}")
        print(f"control    : {args.control_host}:{args.control_port}")
        print(f"runs       : {args.runs}")
        print(f"batches    : {', '.join(labels)}")
        print(f"output_dir : {out_dir}")

        try:
            for label in labels:
                print()
                print(f"waiting for batch {label}...")
                conn, addr = listener.accept()
                with conn:
                    batch_label, batch_runs = parse_batch_start(recv_line(conn))
                    if batch_label != label:
                        raise RuntimeError(
                            f"expected batch {label}, got {batch_label!r}"
                        )
                    if batch_runs != args.runs:
                        raise RuntimeError(
                            f"expected {args.runs} runs for {label}, got {batch_runs}"
                        )

                    print(f"client     : {addr[0]}:{addr[1]}")
                    send_line(conn, "READY")
                    intervals = handle_server_batch(
                        conn,
                        out_dir,
                        label,
                        size_by_label[label],
                        args.runs,
                    )

                metrics = BatchMetrics(
                    label=label,
                    size_bytes=size_by_label[label],
                    runs=args.runs,
                    intervals=intervals,
                )
                metrics_by_batch.append(metrics)
                print(
                    f"{label} single runs: "
                    + ", ".join(f"{value:.3f}s" for value in intervals)
                )
                print(
                    f"{label} batch: total={sum(intervals):.3f}s "
                    f"avg={statistics.mean(intervals):.3f}s "
                    f"median={statistics.median(intervals):.3f}s "
                    f"best={min(intervals):.3f}s"
                )
        finally:
            listener.close()
            server.stop(timeout=10.0)

    print_summary(metrics_by_batch)
    if args.results_dir is not None:
        saved_path = write_results_file(
            Path(args.results_dir), format_summary(metrics_by_batch)
        )
        print(f"results saved: {saved_path}")
    return 0


def run_client(args: argparse.Namespace) -> int:
    global CURRENT_BATCH_DIR
    hf_path = resolve_hf_path()
    sizes = parse_sizes(args.sizes)
    labels = [human_size(size) for size in sizes]

    previous_int, previous_term = install_cleanup_handlers()
    try:
        with tempfile.TemporaryDirectory(prefix="hf_perf_client_") as tmp_dir:
            in_dir = Path(tmp_dir) / "inputs"
            in_dir.mkdir(parents=True, exist_ok=True)

            print("mode       : perf-client")
            print(f"server     : {args.server_host}:{args.hf_port}")
            print(f"control    : {args.server_host}:{args.control_port}")
            print(f"runs       : {args.runs}")
            print(f"rand_seed  : {args.random_seed}")
            print("wash_step  : enabled (same-size sequential read)")
            print(f"batches    : {', '.join(labels)}")

            for size, label in zip(sizes, labels, strict=True):
                size_in_dir = in_dir / label
                size_in_dir.mkdir(parents=True, exist_ok=True)
                CURRENT_BATCH_DIR = size_in_dir
                timeout = transfer_timeout_seconds(size)
                wash_path = size_in_dir / f"wash_{label}.bin"
                write_generated_file(
                    wash_path,
                    size,
                    file_seed(label, 0, args.random_seed),
                )

                print()
                print(
                    f"Sending batch {label} "
                    f"({args.runs} files, timeout={timeout:.1f}s per file)"
                )
                try:
                    with socket.create_connection(
                        (args.server_host, args.control_port), timeout=30.0
                    ) as conn:
                        send_line(conn, f"BATCH {label} {args.runs}")
                        expect_line(
                            recv_line(conn), "READY", f"batch start for {label}"
                        )

                        for run_index in range(1, args.runs + 1):
                            src = (
                                size_in_dir / f"payload_{label}_run{run_index:02d}.bin"
                            )
                            write_generated_file(
                                src,
                                size,
                                file_seed(label, run_index, args.random_seed),
                            )
                            run_wash_step(wash_path)
                            send_line(conn, f"FILE {src.name} {size}")
                            expect_line(
                                recv_line(conn),
                                "READY",
                                f"file start for {src.name}",
                            )
                            try:
                                result = run_hf(
                                    hf_path,
                                    [
                                        "-c",
                                        src,
                                        "-i",
                                        args.server_host,
                                        "-p",
                                        str(args.hf_port),
                                    ],
                                    timeout=timeout,
                                )
                            finally:
                                src.unlink(missing_ok=True)

                            if result.returncode != 0:
                                raise RuntimeError(
                                    "client failed: "
                                    f"argv={result.argv} stdout={result.stdout!r} stderr={result.stderr!r}"
                                )
                            expect_line(
                                recv_line(conn),
                                f"DONE {src.name}",
                                f"file completion for {src.name}",
                            )

                        send_line(conn, "END")
                        expect_line(recv_line(conn), "DONE", f"batch end for {label}")

                    print(f"  completed batch {label}")
                finally:
                    cleanup_current_batch_dir()
    finally:
        restore_cleanup_handlers(previous_int, previous_term)

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Manual HFile transfer benchmark")
    subparsers = parser.add_subparsers(dest="mode", required=True)

    server_parser = subparsers.add_parser("server", help="Run benchmark server")
    server_parser.add_argument(
        "--hf-port", type=int, default=8090, help="hf server port"
    )
    server_parser.add_argument(
        "--control-host", default="0.0.0.0", help="control listener host"
    )
    server_parser.add_argument(
        "--control-port", type=int, default=8091, help="control listener port"
    )
    server_parser.add_argument(
        "--runs", type=int, default=DEFAULT_RUNS, help="files per batch"
    )
    server_parser.add_argument(
        "--sizes",
        default=None,
        help="comma-separated sizes, e.g. 64MiB,256MiB,1GiB,4GiB",
    )
    server_parser.add_argument(
        "--results-dir",
        default=None,
        help="directory for timestamped summary output",
    )
    server_parser.set_defaults(func=run_server)

    client_parser = subparsers.add_parser("client", help="Run benchmark client")
    client_parser.add_argument(
        "--server-host", required=True, help="benchmark server host"
    )
    client_parser.add_argument(
        "--hf-port", type=int, default=8090, help="hf server port"
    )
    client_parser.add_argument(
        "--control-port", type=int, default=8091, help="benchmark control port"
    )
    client_parser.add_argument(
        "--runs", type=int, default=DEFAULT_RUNS, help="files per batch"
    )
    client_parser.add_argument(
        "--sizes",
        default=None,
        help="comma-separated sizes, e.g. 64MiB,256MiB,1GiB,4GiB",
    )
    client_parser.add_argument(
        "--random-seed",
        type=int,
        default=12345,
        help="seed for deterministic file generation",
    )
    client_parser.set_defaults(func=run_client)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
