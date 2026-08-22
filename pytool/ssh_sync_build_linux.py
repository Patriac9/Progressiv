"""Deploy Progressiv to Linux host over SSH/SFTP and try Release build.

Password: env SSH_PASSWORD, or first line of 新建 Text Document.txt
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import paramiko
from paramiko.ssh_exception import SSHException

LOCAL = Path(__file__).resolve().parents[1]
CREDS = LOCAL / "新建 Text Document.txt"

HOST = os.environ.get("SSH_HOST", "139.162.96.188")
USER = os.environ.get("SSH_USER", "root")
PASSWORD = os.environ.get("SSH_PASSWORD", "")
REMOTE = os.environ.get("SSH_PROJECT", "/root/Progressiv").replace("\\", "/").rstrip("/")
BUILD = os.environ.get("SSH_BUILD", f"{REMOTE}/cmake-build-release").replace("\\", "/").rstrip("/")
PUT_RETRIES = 5

SYNC_GLOBS = [
    "CMakeLists.txt",
    "main.cpp",
    "include/*.h",
    "src/*.cpp",
    "model/param.mod",
    "model/*",
    "instId.cfg",
    "app.cfg",
]


def load_password() -> str:
    if PASSWORD:
        return PASSWORD
    if not CREDS.exists():
        raise SystemExit(f"missing password: set SSH_PASSWORD or create {CREDS}")
    return CREDS.read_text(encoding="utf-8", errors="replace").splitlines()[0].strip()


def connect(password: str) -> paramiko.SSHClient:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST,
        username=USER,
        password=password,
        timeout=30,
        allow_agent=False,
        look_for_keys=False,
        banner_timeout=30,
        auth_timeout=30,
    )
    t = client.get_transport()
    if t:
        t.set_keepalive(15)
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
    remote_dir = remote_dir.replace("\\", "/")
    parts = [p for p in remote_dir.split("/") if p]
    cur = ""
    for part in parts:
        cur += "/" + part
        try:
            sftp.stat(cur)
        except OSError:
            sftp.mkdir(cur)


def put_with_retry(client: paramiko.SSHClient, password: str, local: Path, remote: str) -> paramiko.SSHClient:
    remote = remote.replace("\\", "/")
    last_err: Exception | None = None
    for attempt in range(1, PUT_RETRIES + 1):
        try:
            t = client.get_transport()
            if t is None or not t.is_active():
                client = connect(password)
            sftp = client.open_sftp()
            try:
                ensure_remote_dir(sftp, remote.rsplit("/", 1)[0])
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
        except (SSHException, EOFError, OSError) as ex:
            last_err = ex
            print(f"  put failed ({local.name}) {attempt}/{PUT_RETRIES}: {ex}", flush=True)
            try:
                client.close()
            except Exception:
                pass
            time.sleep(min(2 * attempt, 8))
            client = connect(password)
    raise RuntimeError(f"upload failed {local}: {last_err}")


def main() -> int:
    password = load_password()
    client = connect(password)
    print(f"connected {USER}@{HOST}", flush=True)

    run(client, f"mkdir -p '{REMOTE}' '{BUILD}'")
    files = collect_files()
    print(f"syncing {len(files)} files -> {REMOTE}", flush=True)
    for local in files:
        rel = local.relative_to(LOCAL).as_posix()
        remote = f"{REMOTE}/{rel}"
        print(f"  {rel}", flush=True)
        client = put_with_retry(client, password, local, remote)

    cmds = [
        f'cmake -S "{REMOTE}" -B "{BUILD}" -DCMAKE_BUILD_TYPE=Release',
        f'cmake --build "{BUILD}" -j"$(nproc)"',
        f'ls -la "{BUILD}/Progressiv" || true',
    ]
    ok = True
    for cmd in cmds:
        if run(client, cmd) != 0:
            ok = False
            break
    client.close()
    if not ok:
        print("\nBuild failed. Sources are on remote at", REMOTE, flush=True)
        return 1
    print("remote sync+Release build OK", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
