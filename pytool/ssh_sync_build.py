"""Sync local Progressiv sources to the remote Windows box over SSH/SFTP, then build.
Sync local Progressiv sources to the remote Windows box over SSH/SFTP, then build.

Usage (PowerShell):
  $env:SSH_HOST='5.104.83.112'
  $env:SSH_USER='Administrator'
  $env:SSH_PASSWORD='...'
  py pytool/ssh_sync_build.py

Optional:
  $env:SSH_PROJECT=C:\\Users\\Administrator\\Progressiv
  $env:SSH_BUILD=C:\\Users\\Administrator\\Progressiv\\cmake-build-debug
  $env:SSH_SYNC_ONLY=1   # upload only, skip cmake build
"""


from __future__ import annotations

import os
import sys
from pathlib import Path

import paramiko

HOST = os.environ.get("SSH_HOST", "5.104.83.112")
PORT = int(os.environ.get("SSH_PORT", "22"))
PASSWORD = os.environ.get("SSH_PASSWORD", "Am060033")
USER = os.environ.get("SSH_USER", "Administrator")
REMOTE = Path(os.environ.get("SSH_PROJECT", r"C:\Users\Administrator\Progressiv"))
BUILD = Path(os.environ.get("SSH_BUILD", r"C:\Users\Administrator\Progressiv\cmake-build-debug"))
MSYS = r"C:\msys64\ucrt64\bin"
SYNC_ONLY = os.environ.get("SSH_SYNC_ONLY", "").strip() in {"1", "true", "TRUE", "yes"}
LOCAL = Path(__file__).resolve().parents[1]

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
        timeout=20,
        allow_agent=False,
        look_for_keys=False,
        banner_timeout=20,
        auth_timeout=20,
    )
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


def sync(client: paramiko.SSHClient) -> None:
    sftp = client.open_sftp()
    files = collect_files()
    print(f"syncing {len(files)} files -> {REMOTE}", flush=True)
    for local in files:
        rel = local.relative_to(LOCAL)
        remote = str(REMOTE / rel)
        ensure_remote_dir(sftp, str((REMOTE / rel).parent))
        print(f"  {rel}", flush=True)
        sftp.put(str(local), remote)
    sftp.close()


def main() -> int:
    if not PASSWORD:
        print("Set SSH_PASSWORD first.", file=sys.stderr)
        return 2

    client = connect()
    print(f"connected {USER}@{HOST}", flush=True)
    sync(client)

    if SYNC_ONLY:
        client.close()
        print("remote sync OK (build skipped)", flush=True)
        return 0

    path = f"set PATH={MSYS};%PATH%"
    cmds = [
        f'{path} && cmake -S "{REMOTE}" -B "{BUILD}" -G "MinGW Makefiles" '
        f'-DCMAKE_CXX_COMPILER={MSYS}\\g++.exe -DCMAKE_C_COMPILER={MSYS}\\gcc.exe '
        f"-DCMAKE_BUILD_TYPE=Debug",
        f'{path} && cmake --build "{BUILD}" -j',
        f'dir "{BUILD}\\Progressiv.exe"',
    ]
    for cmd in cmds:
        if run(client, cmd) != 0:
            client.close()
            return 1
    client.close()
    print("remote sync+build OK", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
