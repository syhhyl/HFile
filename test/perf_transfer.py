from __future__ import annotations

import argparse
import os
import shutil
import socket
import statistics
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from test.support.hf import HFileServer, resolve_hf_path, run_hf


MIB = 1024 * 1024
GIB = 1024 * MIB
DEFAULT_SIZES = [64 * MIB, 256 * MIB, 1 * GIB, 4 * GIB]
DEFAULT_RUNS = 5
FILE_CHUNK_SIZE = 1 * MIB
FILE_PATTERN = bytes(range(256)) * (FILE_CHUNK_SIZE // 256)
SCAN_INTERVAL = 0.05
TRANSFER_TIMEOUT_SCALE = 30.0
TRANSFER_TIMEOUT_MIN = 30.0


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


def ensure_input_file(path: Path, size: int) -> None:
    if path.is_file() and path.stat().st_size == size:
        return

    if path.exists():
        path.unlink()

    print(f"Preparing input file: {path.name} ({human_size(size)})")
    remaining = size
    with path.open("wb") as f:
        while remaining > 0:
            chunk = FILE_PATTERN
            if remaining < len(chunk):
                chunk = chunk[:remaining]
            f.write(chunk)
            remaining -= len(chunk)


def ensure_batch_inputs(in_dir: Path, label: str, size: int, runs: int) -> list[Path]:
    base = in_dir / f"payload_{label}.bin"
    ensure_input_file(base, size)

    sources: list[Path] = []
    for run_index in range(1, runs + 1):
        path = in_dir / f"payload_{label}_run{run_index:02d}.bin"
        if path.exists() or path.is_symlink():
            path.unlink()

        try:
            os.link(base, path)
        except OSError:
            try:
                os.symlink(base.name, path)
            except OSError:
                shutil.copyfile(base, path)
        sources.append(path)

    return sources


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
    print()
    print("batch     runs  total(s)  avg(s)    median(s)  best(s)  avg(MiB/s)")
    for metrics in metrics_by_batch:
        intervals = metrics.intervals
        total = sum(intervals)
        throughput = ((metrics.size_bytes * metrics.runs) / MIB) / total
        print(
            f"{metrics.label:<9} {metrics.runs:<5} {total:<9.3f} "
            f"{statistics.mean(intervals):<9.3f} "
            f"{statistics.median(intervals):<10.3f} "
            f"{min(intervals):<8.3f} {throughput:.2f}"
        )


def collect_batch_metrics(
    conn: socket.socket,
    out_dir: Path,
    expected_size: int,
    runs: int,
    timeout: float,
    label: str,
) -> list[float]:
    start = time.monotonic()
    conn.setblocking(False)
    known_files = {path.name for path in out_dir.iterdir() if path.is_file()}
    stable_sizes: dict[str, int] = {}
    completed: list[float] = []
    completed_names: set[str] = set()
    end_seen = False
    deadline = start + timeout * runs + 10.0

    while time.monotonic() < deadline:
        for path in out_dir.iterdir():
            if not path.is_file():
                continue
            if path.name in known_files or path.name in completed_names:
                continue

            try:
                size = path.stat().st_size
            except FileNotFoundError:
                continue

            previous = stable_sizes.get(path.name)
            stable_sizes[path.name] = size
            if size != expected_size:
                continue
            if previous != size:
                continue

            completed_names.add(path.name)
            known_files.add(path.name)
            completed.append(time.monotonic())
            if len(completed) == runs and end_seen:
                break

        if len(completed) == runs and end_seen:
            break

        try:
            data = conn.recv(64)
            if data == b"":
                raise RuntimeError("control connection closed before batch end")
            if b"\n" in data:
                end_seen = True
                if len(completed) == runs:
                    break
        except BlockingIOError:
            pass

        time.sleep(SCAN_INTERVAL)

    conn.setblocking(True)

    if not end_seen:
        raise RuntimeError(f"missing batch end marker for {label}")
    if len(completed) != runs:
        raise RuntimeError(
            f"expected {runs} completed files for {label}, got {len(completed)}"
        )

    intervals: list[float] = []
    previous = start
    for finished_at in completed:
        intervals.append(finished_at - previous)
        previous = finished_at
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
                    batch_label = recv_line(conn).strip()
                    if batch_label != label:
                        raise RuntimeError(
                            f"expected batch {label}, got {batch_label!r}"
                        )

                    print(f"client     : {addr[0]}:{addr[1]}")
                    send_line(conn, "")
                    intervals = collect_batch_metrics(
                        conn,
                        out_dir,
                        size_by_label[label],
                        args.runs,
                        transfer_timeout_seconds(size_by_label[label]),
                        label,
                    )
                    send_line(conn, "")

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
    return 0


def run_client(args: argparse.Namespace) -> int:
    hf_path = resolve_hf_path()
    sizes = parse_sizes(args.sizes)
    labels = [human_size(size) for size in sizes]

    with tempfile.TemporaryDirectory(prefix="hf_perf_client_") as tmp_dir:
        in_dir = Path(tmp_dir) / "inputs"
        in_dir.mkdir(parents=True, exist_ok=True)

        print("mode       : perf-client")
        print(f"server     : {args.server_host}:{args.hf_port}")
        print(f"control    : {args.server_host}:{args.control_port}")
        print(f"runs       : {args.runs}")
        print(f"batches    : {', '.join(labels)}")

        for size, label in zip(sizes, labels, strict=True):
            sources = ensure_batch_inputs(in_dir, label, size, args.runs)
            timeout = transfer_timeout_seconds(size)

            print()
            print(
                f"Sending batch {label} "
                f"({args.runs} files, timeout={timeout:.1f}s per file)"
            )

            with socket.create_connection(
                (args.server_host, args.control_port), timeout=30.0
            ) as conn:
                send_line(conn, label)
                recv_line(conn)

                for src in sources:
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
                    if result.returncode != 0:
                        raise RuntimeError(
                            "client failed: "
                            f"argv={result.argv} stdout={result.stdout!r} stderr={result.stderr!r}"
                        )

                send_line(conn, "")
                recv_line(conn)

            print(f"  completed batch {label}")

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
    client_parser.set_defaults(func=run_client)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
