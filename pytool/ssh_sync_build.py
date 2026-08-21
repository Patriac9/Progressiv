"""Sync local Progressiv sources to the remote Windows box over SSH/SFTP, then build.

Usage (PowerShell):
  $env:SSH_HOST='5.104.83.112'
  $env:SSH_USER='Administrator'
  $env:SSH_PASSWORD='...'
  py pytool/ssh_sync_build.py

Optional:
  $env:SSH_PROJECT=C:\\Users\\Administrator\\Progressiv
  $env:SSH_BUILD=C:\\Users\\Administrator\\Progressiv\\cmake-build-release
  $env:SSH_SYNC_ONLY=1   # upload only, skip cmake build
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import paramiko
from paramiko.ssh_exception import SSHException

HOST = os.environ.get("SSH_HOST", "5.104.83.112")
PORT = int(os.environ.get("SSH_PORT", "22"))
PASSWORD = os.environ.get("SSH_PASSWORD", "Am060033")
USER = os.environ.get("SSH_USER", "Administrator")
REMOTE = Path(os.environ.get("SSH_PROJECT", r"C:\Users\Administrator\Progressiv"))
BUILD = Path(os.environ.get("SSH_BUILD", r"C:\Users\Administrator\Progressiv\cmake-build-release"))
MSYS = r"C:\msys64\ucrt64\bin"
SYNC_ONLY = os.environ.get("SSH_SYNC_ONLY", "").strip() in {"1", "true", "TRUE", "yes"}
LOCAL = Path(__file__).resolve().parents[1]
PUT_RETRIES = int(os.environ.get("SSH_PUT_RETRIES", "5"))

SYNC_GLOBS = [
    "CMakeLists.txt",
    ".gitignore",
    "main.cpp",
    "include/*.h",
    "src/*.cpp",
    "model/param.mod",
    "model/*",
    "instId.cfg",
    "app.cfg",
]


def connect() -> paramiko.SSHClient:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST,
        port=PORT,
        username=USER,
        password=PASSWORD,
        timeout=30,
        allow_agent=False,
        look_for_keys=False,
        banner_timeout=30,
        auth_timeout=30,
    )
    # 防止长时间传大文件时被中间设备掐断空闲连接
    transport = client.get_transport()
    if transport is not None:
        transport.set_keepalive(15)
    return client


def run(client: paramiko.SSHClient, cmd: str, timeout: int = 600) -> int:
    print(f"$ {cmd}", flush=True)
    _, stdout, stderr = client.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    code = stdout.channel.recv_exit_status()
    if out:
        print(out, end="" if out.endswith("\n") else "\n", flush=True)
    if err:
        print(err, end="" if err.endswith("\n") else "\n", file=sys.stderr, flush=True)
    print(f"[exit {code}]", flush=True)
    return code


def collect_files() -> list[Path]:
    files: list[Path] = []
    for pattern in SYNC_GLOBS:
        for p in LOCAL.glob(pattern):
            if p.is_file():
                files.append(p)
    return sorted(set(files))


def ensure_remote_dir(sftp: paramiko.SFTPClient, remote_dir: str) -> None:
    parts = Path(remote_dir).parts
    cur = parts[0] + "\\" if parts[0].endswith(":") else parts[0]
    for part in parts[1:]:
        cur = str(Path(cur) / part)
        try:
            sftp.stat(cur)
        except OSError:
            sftp.mkdir(cur)


def _alive(client: paramiko.SSHClient) -> bool:
    t = client.get_transport()
    return t is not None and t.is_active()


def put_with_retry(client: paramiko.SSHClient, local: Path, remote: str) -> paramiko.SSHClient:
    """Upload one file; on drop, reconnect and retry. Returns (possibly new) client."""
    last_err: Exception | None = None
    for attempt in range(1, PUT_RETRIES + 1):
        try:
            if not _alive(client):
                print(f"  reconnecting SSH before {local.name}...", flush=True)
                try:
                    client.close()
                except Exception:
                    pass
                client = connect()
            sftp = client.open_sftp()
            try:
                ensure_remote_dir(sftp, str(Path(remote).parent))
                # 较大文件用较小窗口，降低单次写失败概率
                with sftp.file(remote, "wb") as rf:
                    rf.set_pipelined(True)
                    with open(local, "rb") as lf:
                        while True:
                            chunk = lf.read(32 * 1024)
                            if not chunk:
                                break
                            rf.write(chunk)
            finally:
                try:
                    sftp.close()
                except Exception:
                    pass
            return client
        except (SSHException, EOFError, OSError, ConnectionError) as ex:
            last_err = ex
            print(f"  put failed ({local.name}) attempt {attempt}/{PUT_RETRIES}: {ex}", flush=True)
            try:
                client.close()
            except Exception:
                pass
            time.sleep(min(2 * attempt, 8))
            client = connect()
    raise RuntimeError(f"failed to upload {local} after {PUT_RETRIES} tries: {last_err}")


def sync(client: paramiko.SSHClient) -> paramiko.SSHClient:
    files = collect_files()
    print(f"syncing {len(files)} files -> {REMOTE}", flush=True)
    for local in files:
        rel = local.relative_to(LOCAL)
        remote = str(REMOTE / rel)
        print(f"  {rel}", flush=True)
        client = put_with_retry(client, local, remote)
    return client


def main() -> int:
    if not PASSWORD:
        print("Set SSH_PASSWORD first.", file=sys.stderr)
        return 2

    client = connect()
    print(f"connected {USER}@{HOST}", flush=True)
    try:
        client = sync(client)

        if SYNC_ONLY:
            print("remote sync OK (build skipped)", flush=True)
            return 0

        # 编译前先停掉可能占用 exe 的进程
        run(
            client,
            'powershell -NoProfile -Command "Get-Process Progressiv -ErrorAction SilentlyContinue | Stop-Process -Force"',
        )

        path = f"set PATH={MSYS};%PATH%"
        cmds = [
            f'{path} && cmake -S "{REMOTE}" -B "{BUILD}" -G "MinGW Makefiles" '
            f'-DCMAKE_CXX_COMPILER={MSYS}\\g++.exe -DCMAKE_C_COMPILER={MSYS}\\gcc.exe '
            f"-DCMAKE_BUILD_TYPE=Release",
            f'{path} && cmake --build "{BUILD}" -j',
            f'dir "{BUILD}\\Progressiv.exe"',
        ]
        for cmd in cmds:
            if not _alive(client):
                client = connect()
            if run(client, cmd) != 0:
                return 1
        print("remote sync+build OK", flush=True)
        return 0
    finally:
        try:
            client.close()
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
