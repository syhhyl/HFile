from __future__ import annotations

import hashlib
import ast
import os
import re
import socket
import subprocess
import tempfile
import time
from functools import lru_cache
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


def resolve_hf_path(
    root: os.PathLike[str] | str | None = None,
    *,
    target_os: str | None = None,
) -> Path:
    """Resolve the hf binary path.

    Prefer the runnable repo-local binary for the requested target OS.
    """

    search_root = Path(root) if root is not None else Path.cwd()
    resolved_target = target_os
    if resolved_target is None:
        resolved_target = "windows" if os.name == "nt" else "posix"

    if resolved_target == "windows":
        candidates = [
            search_root / "build" / "hf.exe",
            search_root / "build" / "hf",
        ]
    elif resolved_target == "posix":
        candidates = [search_root / "build" / "hf"]
    else:
        raise ValueError(f"unsupported target_os: {resolved_target}")

    for candidate in candidates:
        if candidate.is_file():
            return candidate

    raise FileNotFoundError(
        "hf binary not found. Build with ./build.sh or cmake --build build"
    )


def reserve_free_port(host: str = "127.0.0.1", attempts: int = 32) -> int:
    """Return a likely-free TCP port.

    Note: this cannot fully prevent a race with other processes, so callers
    should be prepared to retry starting the server.
    """

    last_err: Exception | None = None
    for _ in range(max(1, attempts)):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind((host, 0))
            port = int(s.getsockname()[1])
            return port
        except OSError as e:
            last_err = e
        finally:
            try:
                s.close()
            except Exception:
                pass

    if last_err is not None:
        raise last_err
    raise RuntimeError("failed to reserve a free port")


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(chunk_size), b""):
            h.update(chunk)
    return h.hexdigest()


def tail_text_file(path: Path, max_bytes: int = 32 * 1024) -> str:
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return ""
    if len(data) > max_bytes:
        data = data[-max_bytes:]
    try:
        return data.decode("utf-8", errors="replace")
    except Exception:
        return repr(data)


def default_daemon_log_path() -> Path:
    tmpdir = os.environ.get("TMPDIR") or "/tmp"
    return Path(tmpdir) / "hf-daemon.log"


def wait_for_text_in_file(
    path: Path,
    needle: str,
    *,
    offset: int = 0,
    timeout: float = 5.0,
    interval: float = 0.05,
) -> str | None:
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        try:
            data = path.read_bytes()
        except FileNotFoundError:
            data = b""

        text = data[offset:].decode("utf-8", errors="replace")
        if needle in text:
            return text

        time.sleep(interval)

    return None


def _eval_int_expr(expr: str) -> int:
    normalized = re.sub(
        r"0x[0-9A-Fa-f]+[uUlL]*|\d+[uUlL]*",
        lambda m: re.sub(r"[uUlL]+$", "", m.group(0)),
        expr,
    )
    node = ast.parse(normalized, mode="eval")

    def visit(n: ast.AST) -> int:
        if isinstance(n, ast.Expression):
            return visit(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, int):
            return int(n.value)
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.UAdd, ast.USub)):
            value = visit(n.operand)
            return value if isinstance(n.op, ast.UAdd) else -value
        if isinstance(n, ast.BinOp):
            left = visit(n.left)
            right = visit(n.right)
            if isinstance(n.op, ast.Add):
                return left + right
            if isinstance(n.op, ast.Sub):
                return left - right
            if isinstance(n.op, ast.Mult):
                return left * right
            if isinstance(n.op, ast.FloorDiv):
                return left // right
            if isinstance(n.op, ast.Div):
                return left // right
            if isinstance(n.op, ast.Mod):
                return left % right
            if isinstance(n.op, ast.LShift):
                return left << right
            if isinstance(n.op, ast.RShift):
                return left >> right
            if isinstance(n.op, ast.BitOr):
                return left | right
            if isinstance(n.op, ast.BitAnd):
                return left & right
            if isinstance(n.op, ast.BitXor):
                return left ^ right

        raise ValueError(f"unsupported integer expression: {expr!r}")

    return visit(node)


@lru_cache(maxsize=1)
def _protocol_defines() -> dict[str, int]:
    header_path = Path(__file__).resolve().parents[2] / "src" / "protocol.h"
    text = header_path.read_text(encoding="utf-8")
    values: dict[str, int] = {}

    for line in text.splitlines():
        m = re.match(r"^#define\s+(HF_[A-Z0-9_]+)\s+(.+?)\s*$", line)
        if m is None:
            continue
        name, expr = m.groups()
        expr = expr.split("//", 1)[0].strip()
        if not expr:
            continue
        values[name] = _eval_int_expr(expr)

    return values


def protocol_define(name: str) -> int:
    try:
        return _protocol_defines()[name]
    except KeyError as e:
        raise KeyError(f"protocol define not found: {name}") from e


@dataclass(frozen=True)
class RunResult:
    argv: list[str]
    returncode: int
    stdout: str
    stderr: str


def run_hf(
    hf_path: Path,
    args: Sequence[os.PathLike[str] | str],
    *,
    timeout: float = 10.0,
    env: dict[str, str] | None = None,
    cwd: Path | None = None,
) -> RunResult:
    argv = [str(hf_path), *[str(a) for a in args]]
    p = subprocess.run(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        cwd=str(cwd) if cwd is not None else None,
        env=(os.environ | env) if env is not None else None,
    )
    return RunResult(
        argv=argv, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr
    )


def wait_for_file_stable(
    path: Path,
    *,
    timeout: float = 5.0,
    interval: float = 0.05,
    stable_rounds: int = 3,
) -> bool:
    deadline = time.monotonic() + timeout
    last_size: int | None = None
    stable = 0
    while time.monotonic() < deadline:
        try:
            st = path.stat()
            if not path.is_file():
                raise FileNotFoundError
            size = int(st.st_size)
            if last_size is not None and size == last_size:
                stable += 1
                if stable >= max(1, stable_rounds):
                    return True
            else:
                stable = 0
                last_size = size
        except FileNotFoundError:
            stable = 0
            last_size = None

        time.sleep(interval)
    return False


def assert_files_equal(
    testcase,
    src: Path,
    dst: Path,
    *,
    large_threshold: int = 4 * 1024 * 1024,
) -> None:
    s1 = src.stat().st_size
    s2 = dst.stat().st_size
    testcase.assertEqual(s1, s2, f"size mismatch: {s1} != {s2}")
    if s1 <= large_threshold:
        testcase.assertEqual(
            src.read_bytes(), dst.read_bytes(), "content mismatch (bytes)"
        )
    else:
        testcase.assertEqual(
            sha256_file(src), sha256_file(dst), "content mismatch (sha256)"
        )


def make_temp_dir(prefix: str = "hf_test_") -> tempfile.TemporaryDirectory[str]:
    return tempfile.TemporaryDirectory(prefix=prefix)


class HFileServer:
    """Manage an hf server process.

    The server is started with stdout/stderr redirected to a log file. Readiness
    is determined by waiting until the configured TCP port accepts connections.
    """

    def __init__(
        self,
        *,
        hf_path: Path,
        out_dir: Path,
        host: str = "127.0.0.1",
        port: int | None = None,
        log_path: Path | None = None,
        extra_args: Sequence[os.PathLike[str] | str] = (),
    ) -> None:
        self.hf_path = Path(hf_path)
        self.out_dir = Path(out_dir)
        self.host = host
        self.port = int(port) if port is not None else reserve_free_port(host=host)
        self.log_path = Path(log_path) if log_path is not None else None
        self._startup_log_path: Path | None = None
        self.extra_args = tuple(str(arg) for arg in extra_args)
        self._proc: subprocess.Popen[str] | None = None
        self._log_fh = None
        self._pid: int | None = None

    @property
    def proc(self) -> subprocess.Popen[str]:
        if self._proc is None:
            raise RuntimeError("server not started")
        return self._proc

    @property
    def pid(self) -> int:
        if self._pid is not None:
            return self._pid
        raise RuntimeError("server pid is not available")

    def start(self, *, startup_timeout: float = 5.0) -> None:
        self.out_dir.mkdir(parents=True, exist_ok=True)

        if self.log_path is None:
            # Keep logs inside the output dir to simplify debugging artifacts.
            self.log_path = self.out_dir / "hf_server.log"

        self._startup_log_path = self.log_path

        self._log_fh = self._startup_log_path.open(
            "w", encoding="utf-8", errors="replace"
        )
        argv = [
            str(self.hf_path),
            "-d",
            str(self.out_dir),
            "-p",
            str(self.port),
        ]
        argv.extend(self.extra_args)

        self._proc = subprocess.Popen(
            argv,
            stdout=self._log_fh,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

        self.wait_ready(timeout=startup_timeout)

    def wait_ready(self, *, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            if self._proc is not None and self._proc.poll() is not None:
                if os.name == "nt" or self._proc.returncode != 0:
                    tail = tail_text_file(self._startup_log_path or Path(""))
                    raise RuntimeError(
                        f"hf server exited early (rc={self._proc.returncode}). log tail:\n{tail}"
                    )

            try:
                with socket.create_connection((self.host, self.port), timeout=0.2):
                    pass
                self._capture_runtime_details()
                return
            except OSError:
                pass

            time.sleep(0.05)

        tail = tail_text_file(self._startup_log_path or Path(""))
        raise TimeoutError(
            f"hf server not ready after {timeout:.1f}s on {self.host}:{self.port}. "
            f"port did not accept connections. "
            f"log tail:\n{tail}"
        )

    def stop(self, *, timeout: float = 3.0) -> None:
        if self._proc is None:
            return

        if os.name != "nt":
            subprocess.run(
                [str(self.hf_path), "stop"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=timeout,
                check=False,
            )
            if self._proc.poll() is None:
                self._proc.wait(timeout=timeout)
        elif self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait(timeout=timeout)

        if self._log_fh is not None:
            try:
                self._log_fh.flush()
            except Exception:
                pass
            try:
                self._log_fh.close()
            except Exception:
                pass
            self._log_fh = None

    def _capture_runtime_details(self) -> None:
        if os.name == "nt":
            if self._proc is not None:
                self._pid = self._proc.pid
            return

        self.log_path = default_daemon_log_path()
        try:
            status = run_hf(self.hf_path, ["status"], timeout=2.0)
        except Exception:
            return

        if status.returncode != 0:
            return

        for line in status.stdout.splitlines():
            if line.startswith("pid: "):
                try:
                    self._pid = int(line.split(":", 1)[1].strip())
                except ValueError:
                    self._pid = None

    def __enter__(self) -> "HFileServer":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()

    @property
    def http_url(self) -> str:
        return f"http://{self.host}:{self.port}"
