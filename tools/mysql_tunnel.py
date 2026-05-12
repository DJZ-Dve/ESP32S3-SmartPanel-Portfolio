#!/usr/bin/env python3
"""
Start a local SSH tunnel for viewing the cloud MySQL database from GUI clients.

Default tunnel:
  127.0.0.1:3307 -> <your-ssh-user>@<your-server-host> -> 127.0.0.1:3306
"""

import argparse
import errno
import os
import socket
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENV_FILE = REPO_ROOT / "server-side files" / ".env"
DEFAULT_SSH_TARGET = "<your-ssh-user>@<your-server-host>"


def load_env_file(path):
    values = {}
    if not path.exists():
        return values

    with path.open("r", encoding="utf-8") as file_obj:
        for raw_line in file_obj:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def local_port_status(host, port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((host, port))
            return "free"
        except OSError as exc:
            if exc.errno == errno.EADDRINUSE:
                return "in_use"
            return "unknown"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Open an SSH tunnel to the cloud MySQL service for DBeaver/Navicat/DataGrip."
    )
    parser.add_argument("--env-file", default=str(DEFAULT_ENV_FILE), help="Path to server .env file.")
    parser.add_argument("--ssh-target", default=DEFAULT_SSH_TARGET, help="SSH target, e.g. user@host.")
    parser.add_argument("--local-host", default="127.0.0.1", help="Local bind host.")
    parser.add_argument("--local-port", type=int, default=3307, help="Local bind port.")
    parser.add_argument("--remote-host", default=None, help="Remote MySQL host as seen by the server.")
    parser.add_argument("--remote-port", type=int, default=None, help="Remote MySQL port as seen by the server.")
    parser.add_argument("--dry-run", action="store_true", help="Print settings and SSH command without running it.")
    return parser.parse_args()


def main():
    args = parse_args()
    env_file = Path(args.env_file).expanduser()
    env_values = load_env_file(env_file)

    mysql_host = args.remote_host or env_values.get("MYSQL_HOST", "127.0.0.1")
    mysql_port = args.remote_port or int(env_values.get("MYSQL_PORT", "3306"))
    mysql_user = env_values.get("MYSQL_USER", "").strip()
    mysql_db = env_values.get("MYSQL_DB", "").strip()

    port_status = local_port_status(args.local_host, args.local_port)
    if not args.dry_run and port_status == "in_use":
        print(f"Local port is already in use: {args.local_host}:{args.local_port}", file=sys.stderr)
        print("Use --local-port 3308 or close the existing tunnel.", file=sys.stderr)
        return 2

    ssh_cmd = [
        "ssh",
        "-o",
        "ExitOnForwardFailure=yes",
        "-o",
        "ServerAliveInterval=60",
        "-N",
        "-L",
        f"{args.local_host}:{args.local_port}:{mysql_host}:{mysql_port}",
        args.ssh_target,
    ]

    print("MySQL SSH tunnel")
    print(f"  SSH target: {args.ssh_target}")
    print(f"  Tunnel: {args.local_host}:{args.local_port} -> {mysql_host}:{mysql_port}")
    if port_status == "in_use":
        print(f"  Warning: local port {args.local_host}:{args.local_port} is already in use.")
    print("")
    print("DBeaver/MySQL connection")
    print(f"  Host: {args.local_host}")
    print(f"  Port: {args.local_port}")
    print(f"  Database: {mysql_db or '(see MYSQL_DB in .env)'}")
    print(f"  Username: {mysql_user or '(see MYSQL_USER in .env)'}")
    print("  Password: use MYSQL_PASS from server-side files/.env")
    print("")
    print("Keep this process running while using the database. Press Ctrl+C to close the tunnel.")
    print("")
    print("SSH command:")
    print("  " + " ".join(ssh_cmd))
    sys.stdout.flush()

    if args.dry_run:
        return 0

    try:
        return subprocess.call(ssh_cmd)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
