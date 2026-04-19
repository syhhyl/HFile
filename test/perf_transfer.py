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
import threading
import time
from datetime import datetime
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from test.support.hf import HFileServer, resolve_hf_path, run_hf


KIB = 1024
MIB = 1024 * 1024
GIB = 1024 * MIB
DEFAULT_SIZES = [256 * KIB, 512 * MIB, 1 * GIB, 2 * GIB]
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


def stop_running_server_if_needed(hf_path: Path) -> None:
    status = run_hf(hf_path, ["status"], timeout=5.0)
    if status.returncode != 0:
        return

    if "status: running" not in status.stdout:
        raise RuntimeError(
            "unexpected status output while checking running server: "
            f"stdout={status.stdout!r} stderr={status.stderr!r}"
        )

    print("detected running HFile server, stopping before perf run")
    stop = run_hf(hf_path, ["stop"], timeout=10.0)
    if stop.returncode != 0:
        raise RuntimeError(
            "failed to stop running HFile server before perf run: "
            f"stdout={stop.stdout!r} stderr={stop.stderr!r}"
        )

    verify = run_hf(hf_path, ["status"], timeout=5.0)
    if verify.returncode == 0:
        raise RuntimeError(
            "running HFile server still present after stop: "
            f"stdout={verify.stdout!r} stderr={verify.stderr!r}"
        )


def resolve_local_perf_host(preferred_host: str | None) -> tuple[str, bool]:
    if preferred_host is not None and preferred_host.strip():
        return preferred_host.strip(), False

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 53))
        host = sock.getsockname()[0]
    except OSError:
        host = "127.0.0.1"
    finally:
        sock.close()

    if host in ("0.0.0.0", "127.0.0.1", ""):
        return "127.0.0.1", True
    return host, False


def recv_line(sock: socket.socket, abort_event: threading.Event | None = None) -> str:
    chunks = bytearray()
    while True:
        try:
            data = sock.recv(1)
        except socket.timeout:
            if abort_event is not None and abort_event.is_set():
                raise RuntimeError("benchmark server aborted")
            continue
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
    abort_event: threading.Event | None = None,
) -> float:
    target = out_dir / file_name
    start = time.monotonic()
    previous_size: int | None = None
    deadline = start + timeout + 10.0

    while time.monotonic() < deadline:
        if abort_event is not None and abort_event.is_set():
            raise RuntimeError("benchmark server aborted")
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
    abort_event: threading.Event | None = None,
) -> list[float]:
    intervals: list[float] = []
    timeout = transfer_timeout_seconds(expected_size)

    for _ in range(runs):
        file_name, file_size = parse_file_start(recv_line(conn, abort_event))
        if file_size != expected_size:
            raise RuntimeError(
                f"expected file size {expected_size} for {label}, got {file_size}"
            )

        send_line(conn, "READY")
        elapsed = wait_for_completed_file(
            out_dir, file_name, expected_size, timeout, abort_event
        )
        intervals.append(elapsed)
        (out_dir / file_name).unlink(missing_ok=True)
        send_line(conn, f"DONE {file_name}")

    expect_line(recv_line(conn, abort_event), "END", f"batch end for {label}")
    send_line(conn, "DONE")
    return intervals


def run_server(args: argparse.Namespace) -> int:
    hf_path = resolve_hf_path()
    stop_running_server_if_needed(hf_path)
    sizes = parse_sizes(args.sizes)
    labels = [human_size(size) for size in sizes]
    size_by_label = dict(zip(labels, sizes, strict=True))
    metrics_by_batch: list[BatchMetrics] = []
    ready_event = getattr(args, "_ready_event", None)
    abort_event = getattr(args, "_abort_event", None)

    with tempfile.TemporaryDirectory(prefix="hf_perf_server_") as tmp_dir:
        out_dir = Path(tmp_dir) / "outputs"
        out_dir.mkdir(parents=True, exist_ok=True)

        server = HFileServer(hf_path=hf_path, out_dir=out_dir, port=args.hf_port)
        server.start(startup_timeout=10.0)

        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.control_host, args.control_port))
        listener.listen(1)
        if abort_event is not None:
            listener.settimeout(0.2)
        if ready_event is not None:
            ready_event.set()

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
                while True:
                    try:
                        conn, addr = listener.accept()
                        break
                    except socket.timeout:
                        if abort_event is not None and abort_event.is_set():
                            raise RuntimeError("benchmark server aborted")
                with conn:
                    if abort_event is not None:
                        conn.settimeout(0.2)
                    batch_label, batch_runs = parse_batch_start(
                        recv_line(conn, abort_event)
                    )
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
                        abort_event,
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


def run_local(args: argparse.Namespace) -> int:
    hf_path = resolve_hf_path()
    stop_running_server_if_needed(hf_path)
    local_host, used_loopback_fallback = resolve_local_perf_host(args.control_host)

    if used_loopback_fallback:
        print("local host : 127.0.0.1 (LAN IPv4 unavailable, using fallback)")
    else:
        print(f"local host : {local_host}")

    ready_event = threading.Event()
    abort_event = threading.Event()
    done_event = threading.Event()
    server_state: dict[str, object] = {"error": None, "returncode": None}

    server_args = argparse.Namespace(
        hf_port=args.hf_port,
        control_host=local_host,
        control_port=args.control_port,
        runs=args.runs,
        sizes=args.sizes,
        results_dir=args.results_dir,
        _ready_event=ready_event,
        _abort_event=abort_event,
    )

    def server_target() -> None:
        try:
            server_state["returncode"] = run_server(server_args)
        except BaseException as exc:
            server_state["error"] = exc
        finally:
            done_event.set()

    server_thread = threading.Thread(target=server_target, name="hf-perf-server")
    server_thread.start()

    deadline = time.monotonic() + 15.0
    while not ready_event.wait(timeout=0.05):
        if done_event.is_set():
            error = server_state["error"]
            if isinstance(error, BaseException):
                raise RuntimeError("perf server failed before ready") from error
            raise RuntimeError("perf server exited before ready")
        if time.monotonic() >= deadline:
            abort_event.set()
            server_thread.join(timeout=5.0)
            raise TimeoutError("timed out waiting for local perf server to become ready")

    client_args = argparse.Namespace(
        server_host=local_host,
        hf_port=args.hf_port,
        control_port=args.control_port,
        runs=args.runs,
        sizes=args.sizes,
        random_seed=args.random_seed,
    )

    try:
        client_rc = run_client(client_args)
    except BaseException:
        abort_event.set()
        server_thread.join(timeout=10.0)
        raise

    server_thread.join(timeout=10.0)
    if server_thread.is_alive():
        abort_event.set()
        server_thread.join(timeout=5.0)
        raise RuntimeError("perf server did not exit after local benchmark finished")

    error = server_state["error"]
    if isinstance(error, BaseException):
        raise RuntimeError("perf server failed during local benchmark") from error

    server_rc = server_state["returncode"]
    if server_rc not in (None, 0):
        return int(server_rc)
    return client_rc


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

    local_parser = subparsers.add_parser("local", help="Run local benchmark end-to-end")
    local_parser.add_argument("--hf-port", type=int, default=8090, help="hf server port")
    local_parser.add_argument(
        "--control-host",
        default=None,
        help="control/listener host; defaults to detected LAN IPv4, fallback 127.0.0.1",
    )
    local_parser.add_argument(
        "--control-port", type=int, default=8091, help="benchmark control port"
    )
    local_parser.add_argument(
        "--runs", type=int, default=DEFAULT_RUNS, help="files per batch"
    )
    local_parser.add_argument(
        "--sizes",
        default=None,
        help="comma-separated sizes, e.g. 64MiB,256MiB,1GiB,4GiB",
    )
    local_parser.add_argument(
        "--results-dir",
        default=None,
        help="directory for timestamped summary output",
    )
    local_parser.add_argument(
        "--random-seed",
        type=int,
        default=12345,
        help="seed for deterministic file generation",
    )
    local_parser.set_defaults(func=run_local)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
