"""Run Progressiv on the remote Linux host and stream logs to this console.

Password: env SSH_PASSWORD, or first line of 新建 Text Document.txt
Ctrl+C stops the remote process.

  py -B pytool/run_remote.py
  run_remote.bat
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import paramiko

LOCAL = Path(__file__).resolve().parents[1]
CREDS = LOCAL / "新建 Text Document.txt"

HOST = os.environ.get("SSH_HOST", "139.162.96.188")
USER = os.environ.get("SSH_USER", "root")
PASSWORD = os.environ.get("SSH_PASSWORD", "")
REMOTE = os.environ.get("SSH_PROJECT", "/root/Progressiv").replace("\\", "/").rstrip("/")
BUILD = os.environ.get("SSH_BUILD", f"{REMOTE}/cmake-build-release").replace("\\", "/").rstrip("/")
BIN = f"{BUILD}/Progressiv"


def load_password() -> str:
    if PASSWORD:
        return PASSWORD
    if not CREDS.exists():
        raise SystemExit(f"missing password: set SSH_PASSWORD or create {CREDS}")
    return CREDS.read_text(encoding="utf-8", errors="replace").splitlines()[0].strip()


def main() -> int:
    password = load_password()
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    print(f"connecting {USER}@{HOST} ...", flush=True)
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
    transport = client.get_transport()
    if transport:
        transport.set_keepalive(15)

    # stop previous instance if any
    client.exec_command("pkill -x Progressiv || true")
    time.sleep(0.5)

    cmd = f"cd '{BUILD}' && exec ./Progressiv"
    print(f"starting: {cmd}", flush=True)
    print("(Ctrl+C to stop remote Progressiv)\n", flush=True)

    chan = client.get_transport().open_session()
    chan.get_pty(term="xterm", width=120, height=40)
    chan.exec_command(cmd)

    try:
        while True:
            if chan.recv_ready():
                data = chan.recv(4096)
                if not data:
                    break
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            if chan.recv_stderr_ready():
                data = chan.recv_stderr(4096)
                if data:
                    sys.stderr.write(data.decode("utf-8", errors="replace"))
                    sys.stderr.flush()
            if chan.exit_status_ready():
                # drain remaining
                while chan.recv_ready():
                    sys.stdout.write(chan.recv(4096).decode("utf-8", errors="replace"))
                while chan.recv_stderr_ready():
                    sys.stderr.write(chan.recv_stderr(4096).decode("utf-8", errors="replace"))
                code = chan.recv_exit_status()
                print(f"\n[remote exit {code}]", flush=True)
                return code
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("\nstopping remote Progressiv ...", flush=True)
        try:
            chan.send("\x03")  # Ctrl+C
            time.sleep(0.3)
        except Exception:
            pass
        client.exec_command("pkill -x Progressiv || true")
        return 130
    finally:
        try:
            chan.close()
        except Exception:
            pass
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
