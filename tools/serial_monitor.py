#!/usr/bin/env python3
"""
Simple serial monitor helper for ESP32 development.

Examples:
  python tools/serial_monitor.py --port COM5
  python tools/serial_monitor.py --port COM5 --duration 5
  python tools/serial_monitor.py --port COM5 --send DIAG --duration 5
  python tools/serial_monitor.py --port COM5 --send BLEACON --send-delay 2 --duration 15
  python tools/serial_monitor.py --port COM5 --send-at 12:LOG=2 --send-at 20:BLEACON
"""

from __future__ import annotations

import argparse
import sys
import time

import serial
from serial.tools import list_ports


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Lightweight serial monitor for ESP32 logs.")
    parser.add_argument("--port", default="COM5", help="Serial port name, e.g. COM5")
    parser.add_argument("--baud", type=int, default=921600, help="Baud rate")
    parser.add_argument(
        "--duration",
        type=float,
        default=0.0,
        help="Stop after N seconds. Use 0 to keep running until Ctrl+C.",
    )
    parser.add_argument(
        "--send",
        action="append",
        default=[],
        help="Optional command to send after the port opens; can be repeated",
    )
    parser.add_argument(
        "--send-delay",
        type=float,
        default=0.0,
        help="Delay in seconds before sending --send",
    )
    parser.add_argument(
        "--send-at",
        action="append",
        default=[],
        metavar="SECONDS:COMMAND",
        help="Schedule a command at N seconds after the port opens; can be repeated",
    )
    parser.add_argument(
        "--eol",
        choices=("lf", "crlf", "cr"),
        default="lf",
        help="Line ending used when sending --send",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=0.2,
        help="Serial read timeout in seconds",
    )
    parser.add_argument(
        "--encoding",
        default="utf-8",
        help="Text encoding for output",
    )
    parser.add_argument(
        "--errors",
        default="replace",
        help="Decode error handler, e.g. replace or ignore",
    )
    parser.add_argument(
        "--log-file",
        default="",
        help="Optional file path to append the captured output",
    )
    parser.add_argument(
        "--strip-ansi",
        action="store_true",
        help="Remove basic ANSI escape sequences from output",
    )
    parser.add_argument(
        "--timestamp",
        action="store_true",
        help="Prefix each output line with local receive time",
    )
    parser.add_argument(
        "--dtr",
        action="store_true",
        help="Assert DTR after opening the serial port.",
    )
    parser.add_argument(
        "--rts",
        action="store_true",
        help="Assert RTS after opening the serial port.",
    )
    parser.add_argument(
        "--list-ports",
        action="store_true",
        help="List available serial ports and exit",
    )
    return parser


def eol_bytes(name: str) -> bytes:
    if name == "crlf":
        return b"\r\n"
    if name == "cr":
        return b"\r"
    return b"\n"


def strip_ansi(text: str) -> str:
    result: list[str] = []
    i = 0
    length = len(text)
    while i < length:
        ch = text[i]
        if ch == "\x1b" and i + 1 < length and text[i + 1] == "[":
            i += 2
            while i < length and text[i] not in "ABCDEFGHJKSTfmnsu":
                i += 1
            if i < length:
                i += 1
            continue
        result.append(ch)
        i += 1
    return "".join(result)


def normalize_eol(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def timestamp_prefix() -> str:
    now = time.time()
    local = time.localtime(now)
    millis = int((now - int(now)) * 1000)
    return time.strftime("[%H:%M:%S", local) + f".{millis:03d}] "


def parse_send_schedule(send_values: list[str], send_delay: float, send_at_values: list[str]) -> list[tuple[float, str]]:
    schedule: list[tuple[float, str]] = []
    for index, command in enumerate(send_values):
        schedule.append((send_delay + index * 0.5, command))
    for item in send_at_values:
        if ":" not in item:
            raise ValueError(f"--send-at must be SECONDS:COMMAND, got {item!r}")
        seconds_text, command = item.split(":", 1)
        seconds = float(seconds_text)
        if seconds < 0:
            raise ValueError(f"--send-at seconds must be non-negative, got {seconds_text!r}")
        if not command:
            raise ValueError("--send-at command must not be empty")
        schedule.append((seconds, command))
    schedule.sort(key=lambda entry: entry[0])
    return schedule


def main() -> int:
    args = build_parser().parse_args()

    if args.list_ports:
        for info in list_ports.comports():
            desc = info.description or ""
            print(f"{info.device} {desc}".rstrip())
        return 0

    try:
        send_schedule = parse_send_schedule(args.send, args.send_delay, args.send_at)
    except ValueError as exc:
        print(f"invalid send schedule: {exc}", file=sys.stderr)
        return 2

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = args.timeout
    ser.parity = serial.PARITY_NONE
    ser.stopbits = serial.STOPBITS_ONE
    ser.bytesize = serial.EIGHTBITS
    ser.rtscts = False
    ser.xonxoff = False
    ser.dsrdtr = False
    ser.dtr = args.dtr
    ser.rts = args.rts

    try:
        ser.open()
    except Exception as exc:
        print(f"open serial failed: {exc}", file=sys.stderr)
        return 1

    log_fp = None
    if args.log_file:
        log_fp = open(args.log_file, "a", encoding="utf-8", newline="")

    try:
        start_ts = time.time()
        next_send_index = 0
        at_line_start = True

        print(f"[monitor] port={args.port} baud={args.baud}", file=sys.stderr)
        if send_schedule:
            print(
                f"[monitor] queued {len(send_schedule)} command(s): {send_schedule}",
                file=sys.stderr,
            )

        while True:
            now = time.time()

            if next_send_index < len(send_schedule):
                due_at, command = send_schedule[next_send_index]
                if now - start_ts >= due_at:
                    payload = command.encode(args.encoding, errors="ignore") + eol_bytes(args.eol)
                    ser.write(payload)
                    ser.flush()
                    next_send_index += 1
                    print(f"[monitor] sent: {command}", file=sys.stderr)

            chunk = ser.read(4096)
            if chunk:
                text = chunk.decode(args.encoding, errors=args.errors)
                text = normalize_eol(text)
                if args.strip_ansi:
                    text = strip_ansi(text)
                if args.timestamp:
                    rendered_parts = []
                    for part in text.splitlines(keepends=True):
                        if at_line_start:
                            rendered_parts.append(timestamp_prefix())
                        rendered_parts.append(part)
                        at_line_start = part.endswith("\n")
                    rendered = "".join(rendered_parts)
                else:
                    rendered = text
                sys.stdout.write(rendered)
                sys.stdout.flush()
                if log_fp is not None:
                    log_fp.write(rendered)
                    log_fp.flush()

            if args.duration > 0 and now - start_ts >= args.duration:
                break
    except KeyboardInterrupt:
        print("\n[monitor] stopped by user", file=sys.stderr)
    finally:
        ser.close()
        if log_fp is not None:
            log_fp.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
