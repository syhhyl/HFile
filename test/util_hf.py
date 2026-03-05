from __future__ import annotations

import hashlib
import os
import socket
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


def resolve_hf_path() -> Path:
    """Resolve the hf binary path.

       build/hf (repo-local)
    """

    build_bin = Path("build") / "hf"
    if build_bin.exists():
        return build_bin

    raise FileNotFoundError(
        "hf binary not found. Build with ./build.sh"
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
    is determined by waiting for the "listening on ..." server log line.
    """

    def __init__(
        self,
        *,
        hf_path: Path,
        out_dir: Path,
        host: str = "127.0.0.1",
        port: int | None = None,
        log_path: Path | None = None,
    ) -> None:
        self.hf_path = Path(hf_path)
        self.out_dir = Path(out_dir)
        self.host = host
        self.port = int(port) if port is not None else reserve_free_port(host=host)
        self.log_path = Path(log_path) if log_path is not None else None
        self._proc: subprocess.Popen[str] | None = None
        self._log_fh = None

    @property
    def proc(self) -> subprocess.Popen[str]:
        if self._proc is None:
            raise RuntimeError("server not started")
        return self._proc

    def start(self, *, startup_timeout: float = 5.0) -> None:
        self.out_dir.mkdir(parents=True, exist_ok=True)

        if self.log_path is None:
            # Keep logs inside the output dir to simplify debugging artifacts.
            self.log_path = self.out_dir / "hf_server.log"

        self._log_fh = self.log_path.open("w", encoding="utf-8", errors="replace")
        argv = [
            str(self.hf_path),
            "-s",
            str(self.out_dir),
            "-p",
            str(self.port),
        ]

        self._proc = subprocess.Popen(
            argv,
            stdout=self._log_fh,
            stderr=subprocess.STDOUT,
            text=True,
        )

        self.wait_ready(timeout=startup_timeout)

    def wait_ready(self, *, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        needle = "listening on "
        last_err: Exception | None = None

        while time.monotonic() < deadline:
            if self._proc is not None and self._proc.poll() is not None:
                tail = tail_text_file(self.log_path or Path(""))
                raise RuntimeError(
                    f"hf server exited early (rc={self._proc.returncode}). log tail:\n{tail}"
                )

            if self.log_path is not None:
                log_text = tail_text_file(self.log_path)
                if needle in log_text:
                    return

            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.settimeout(0.2)
                s.connect((self.host, self.port))
                return
            except OSError as e:
                last_err = e
            finally:
                try:
                    s.close()
                except Exception:
                    pass

            time.sleep(0.05)

        tail = tail_text_file(self.log_path or Path(""))
        raise TimeoutError(
            f"hf server not ready after {timeout:.1f}s on {self.host}:{self.port}. "
            f"expected log line containing {needle!r}; last_error={last_err!r}. "
            f"log tail:\n{tail}"
        )

    def stop(self, *, timeout: float = 3.0) -> None:
        if self._proc is None:
            return

        if self._proc.poll() is None:
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

    def __enter__(self) -> "HFileServer":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()
