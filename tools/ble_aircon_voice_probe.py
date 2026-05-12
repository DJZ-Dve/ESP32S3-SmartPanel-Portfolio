#!/usr/bin/env python3
"""
Probe the BLE air-conditioner voice path from a developer machine.

The tool can:
  - synthesize test utterances with edge-tts,
  - send the 16 kHz / 16-bit / mono PCM to ai_server.py using the ESP32 TCP protocol,
  - parse the returned control JSON and timing,
  - tail the Python server log,
  - watch the ESP32 serial log and optionally execute the returned BLE steps via serial commands.

The fake TCP client can verify ASR/NLU/server output. It cannot make the ESP32 execute BLE by itself,
because BLE execution happens on the ESP32 connection that receives the server response. Use
--execute-ble-via-serial to actuate the same returned steps through the existing serial BLE commands.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - checked at runtime for developer machines.
    serial = None
    list_ports = None


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SERVER_LOG = REPO_ROOT / "server-side files" / "logs" / "server.log"
EOS_MARKER = b"EOS!EOS!"
PCM_SAMPLE_RATE = 16000
PCM_SAMPLE_WIDTH = 2
PCM_CHANNELS = 1
SERIAL_LINE_PATTERNS = {
    "ok": re.compile(r"^\s*OK\s*$"),
    "err": re.compile(r"ERR:\s*(.*)"),
    "ble_write_ok": re.compile(r"BLE air conditioner command write succeeded"),
    "ble_released": re.compile(r"BLE command connection has been released"),
    "rms": re.compile(r"\[Feed\]\s+RMS=(\d+)"),
}


def now_ms() -> int:
    return int(time.time() * 1000)


def elapsed_ms(start: float) -> int:
    return int((time.monotonic() - start) * 1000)


def info(message: str) -> None:
    print(message, flush=True)


def load_wav_as_pcm16k(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        sample_width = wf.getsampwidth()
        rate = wf.getframerate()
        if channels != PCM_CHANNELS or sample_width != PCM_SAMPLE_WIDTH or rate != PCM_SAMPLE_RATE:
            raise ValueError(
                f"{path} must be 16 kHz / 16-bit / mono WAV, got "
                f"{rate} Hz / {sample_width * 8} bit / {channels} ch"
            )
        return wf.readframes(wf.getnframes())


def load_audio_file(path: Path) -> bytes:
    suffix = path.suffix.lower()
    if suffix == ".pcm":
        data = path.read_bytes()
        if len(data) % 2 != 0:
            raise ValueError(f"{path} has odd byte length; expected int16 PCM")
        return data
    if suffix == ".wav":
        return load_wav_as_pcm16k(path)
    raise ValueError(f"unsupported audio file type: {path}")


async def synthesize_edge_tts_mp3(text: str, voice: str, rate: str, volume: str, pitch: str) -> bytes:
    try:
        import edge_tts
    except ImportError as exc:
        raise RuntimeError(
            "edge-tts is not installed. Install it with: python -m pip install edge-tts"
        ) from exc

    communicate = edge_tts.Communicate(text, voice=voice, rate=rate, volume=volume, pitch=pitch)
    chunks: list[bytes] = []
    async for chunk in communicate.stream():
        if chunk.get("type") == "audio":
            chunks.append(chunk["data"])
    if not chunks:
        raise RuntimeError("edge-tts returned no audio")
    return b"".join(chunks)


def decode_mp3_to_pcm16k(mp3_data: bytes) -> bytes:
    with tempfile.TemporaryDirectory(prefix="ble_voice_probe_") as tmp_dir:
        tmp = Path(tmp_dir)
        mp3_path = tmp / "edge_tts.mp3"
        wav_path = tmp / "edge_tts_16k.wav"
        mp3_path.write_bytes(mp3_data)

        if shutil.which("afconvert"):
            cmd = [
                "afconvert",
                str(mp3_path),
                str(wav_path),
                "-f",
                "WAVE",
                "-d",
                "LEI16@16000",
                "-c",
                "1",
            ]
        elif shutil.which("ffmpeg"):
            cmd = [
                "ffmpeg",
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(mp3_path),
                "-ac",
                "1",
                "-ar",
                "16000",
                "-f",
                "wav",
                str(wav_path),
            ]
        else:
            raise RuntimeError("Need afconvert or ffmpeg to decode edge-tts MP3 to 16 kHz PCM")

        subprocess.run(cmd, check=True)
        return load_wav_as_pcm16k(wav_path)


def synthesize_edge_tts_pcm(text: str, voice: str, rate: str, volume: str, pitch: str) -> tuple[bytes, int]:
    start = time.monotonic()
    mp3_data = asyncio.run(synthesize_edge_tts_mp3(text, voice, rate, volume, pitch))
    pcm_data = decode_mp3_to_pcm16k(mp3_data)
    return pcm_data, elapsed_ms(start)


def build_device_meta(ai_pipeline: str = "") -> dict[str, Any]:
    meta = {
        "product_id": "esp32s3_ble_aircon",
        "profile": "ble_only",
        "capabilities": {
            "aircon_ble": True,
            "ir": False,
            "rf433": False,
            "scenes": False,
        },
        "scenes": [],
    }
    if ai_pipeline:
        meta["ai_pipeline"] = ai_pipeline
    return meta


def send_exact(sock: socket.socket, data: bytes) -> None:
    sock.sendall(data)


def recv_until_response(sock: socket.socket, timeout_sec: float) -> tuple[dict[str, Any], bytes, dict[str, int]]:
    start = time.monotonic()
    sock.settimeout(timeout_sec)
    json_hex = bytearray()
    audio = bytearray()
    json_done = False
    first_audio_ms: int | None = None
    json_ms: int | None = None

    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        offset = 0
        if not json_done:
            star_index = chunk.find(b"*")
            if star_index >= 0:
                json_hex.extend(b for b in chunk[:star_index] if b not in (ord("\r"), ord("\n")))
                json_done = True
                json_ms = elapsed_ms(start)
                offset = star_index + 1
            else:
                json_hex.extend(b for b in chunk if b not in (ord("\r"), ord("\n")))
                continue

        if json_done and offset < len(chunk):
            if first_audio_ms is None:
                first_audio_ms = elapsed_ms(start)
            audio.extend(chunk[offset:])
            eos_index = audio.find(EOS_MARKER)
            if eos_index >= 0:
                audio = audio[:eos_index]
                break

    if not json_done:
        raise RuntimeError("server response did not contain JSON terminator '*'")

    try:
        json_bytes = bytes.fromhex(json_hex.decode("ascii"))
        payload = json.loads(json_bytes.decode("utf-8"))
    except Exception as exc:
        raise RuntimeError(f"failed to decode server JSON hex: {json_hex[:120]!r}") from exc

    timings = {
        "json_ms": json_ms if json_ms is not None else elapsed_ms(start),
        "first_audio_ms": first_audio_ms if first_audio_ms is not None else -1,
        "eos_ms": elapsed_ms(start),
        "audio_bytes": len(audio),
    }
    return payload, bytes(audio), timings


def send_audio_to_server(
    host: str,
    port: int,
    device_id: str,
    pcm_data: bytes,
    timeout_sec: float,
    ai_pipeline: str = "",
) -> tuple[dict[str, Any], dict[str, int]]:
    start = time.monotonic()
    with socket.create_connection((host, port), timeout=timeout_sec) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connect_ms = elapsed_ms(start)

        id_bytes = device_id.encode("utf-8")
        send_exact(sock, struct.pack(">I", len(id_bytes)))
        send_exact(sock, id_bytes)

        meta_bytes = json.dumps(build_device_meta(ai_pipeline), ensure_ascii=False).encode("utf-8")
        send_exact(sock, b"META")
        send_exact(sock, struct.pack(">I", len(meta_bytes)))
        send_exact(sock, meta_bytes)

        upload_start = time.monotonic()
        send_exact(sock, struct.pack(">I", len(pcm_data)))
        send_exact(sock, pcm_data)
        upload_ms = elapsed_ms(upload_start)

        payload, _audio, timings = recv_until_response(sock, timeout_sec)

    timings["connect_ms"] = connect_ms
    timings["upload_ms"] = upload_ms
    timings["round_trip_ms"] = elapsed_ms(start)
    return payload, timings


def discover_serial_port() -> str:
    if list_ports is None:
        return ""
    candidates = list(list_ports.comports())
    candidates = [
        p
        for p in candidates
        if "debug-console" not in p.device.lower()
        and "bluetooth-incoming-port" not in p.device.lower()
    ]
    preferred = [
        p.device
        for p in candidates
        if "usbmodem" in p.device.lower()
        or "usbserial" in p.device.lower()
        or "jtag" in (p.description or "").lower()
    ]
    if preferred:
        return preferred[0]
    return candidates[0].device if candidates else ""


class ServerLogTail:
    def __init__(self, path: Path | None, enabled: bool = True):
        self.path = path
        self.enabled = enabled and path.exists()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        if not self.enabled and not self.path:
            return
        if not self.enabled:
            if self.path:
                info(f"[server-log] disabled; file not found: {self.path}")
            return
        self._thread = threading.Thread(target=self._run, name="server-log-tail", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)

    def _run(self) -> None:
        with self.path.open("r", encoding="utf-8", errors="replace") as fp:
            fp.seek(0, os.SEEK_END)
            while not self._stop.is_set():
                line = fp.readline()
                if not line:
                    time.sleep(0.1)
                    continue
                line = line.rstrip("\n")
                on_server_log_line(line)
                info(f"[server-log] {line}")


class SerialMonitor:
    def __init__(self, port: str, baud: int):
        self.port = port
        self.baud = baud
        self.lines: queue.Queue[tuple[int, str]] = queue.Queue()
        self.rms_values: list[int] = []
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._ser: Any = None

    def start(self) -> None:
        if not self.port:
            info("[serial] disabled; no serial port selected")
            return
        if serial is None:
            raise RuntimeError("pyserial is not installed")

        self._ser = serial.Serial(
            port=self.port,
            baudrate=self.baud,
            timeout=0.2,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            bytesize=serial.EIGHTBITS,
            rtscts=False,
            xonxoff=False,
            dsrdtr=False,
        )
        self._ser.dtr = False
        self._ser.rts = False
        info(f"[serial] port={self.port} baud={self.baud}")
        self._thread = threading.Thread(target=self._run, name="serial-monitor", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        if self._ser:
            self._ser.close()

    def send_line(self, text: str) -> None:
        if not self._ser:
            raise RuntimeError("serial monitor is not open")
        self._ser.write(text.encode("utf-8") + b"\n")
        self._ser.flush()
        info(f"[serial<<] {text}")

    def drain_lines(self) -> None:
        while True:
            try:
                self.lines.get_nowait()
            except queue.Empty:
                return

    def wait_ble_result(self, timeout_sec: float) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_sec
        captured: list[str] = []
        saw_write_ok = False
        saw_release = False
        while time.monotonic() < deadline:
            try:
                _ts, line = self.lines.get(timeout=0.2)
            except queue.Empty:
                continue
            captured.append(line)
            if SERIAL_LINE_PATTERNS["ble_write_ok"].search(line):
                saw_write_ok = True
            if SERIAL_LINE_PATTERNS["ble_released"].search(line):
                saw_release = True
            err_match = SERIAL_LINE_PATTERNS["err"].search(line)
            if err_match:
                return {
                    "ok": False,
                    "error": err_match.group(1).strip() or line.strip(),
                    "write_ok": saw_write_ok,
                    "released": saw_release,
                    "lines": captured,
                }
            if SERIAL_LINE_PATTERNS["ok"].match(line):
                return {
                    "ok": True,
                    "error": "",
                    "write_ok": saw_write_ok,
                    "released": saw_release,
                    "lines": captured,
                }
        return {
            "ok": False,
            "error": "serial BLE command timed out",
            "write_ok": saw_write_ok,
            "released": saw_release,
            "lines": captured,
        }

    def _run(self) -> None:
        pending = ""
        while not self._stop.is_set():
            data = self._ser.read(4096)
            if not data:
                continue
            text = data.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")
            pending += text
            while "\n" in pending:
                line, pending = pending.split("\n", 1)
                self._handle_line(line)
        if pending:
            self._handle_line(pending)

    def _handle_line(self, line: str) -> None:
        timestamp = now_ms()
        clean = line.rstrip()
        rms_match = SERIAL_LINE_PATTERNS["rms"].search(clean)
        if rms_match:
            self.rms_values.append(int(rms_match.group(1)))
        on_serial_line(clean)
        info(f"[serial] {clean}")
        self.lines.put((timestamp, clean))


def control_steps_to_serial_commands(control: dict[str, Any]) -> list[str]:
    if not control or not control.get("has_command"):
        return []
    if control.get("protocol") != "aircon_ble_v1":
        return []

    commands: list[str] = []
    for step in control.get("steps") or []:
        step_type = step.get("type")
        if step_type == "power_state":
            power = str(step.get("power") or "").lower()
            if power == "on":
                commands.append("BLEACON")
            elif power == "off":
                commands.append("BLEACOFF")
        elif step_type == "mode_state":
            mode = str(step.get("mode") or "").lower()
            mapping = {"cool": "COOL", "vent": "VENT", "eco": "ECO", "sleep": "SLEEP"}
            if mode in mapping:
                commands.append(f"BLEMODE {mapping[mode]}")
        elif step_type == "temperature_state":
            temp = int(step.get("temp"))
            commands.append(f"BLECTEMP {temp}")
        elif step_type == "fan_state":
            fan = str(step.get("fan") or "")
            if fan:
                commands.append(f"BLEFAN {fan}")
        elif step_type == "display_state":
            display = str(step.get("display") or "").lower()
            if display in ("on", "off"):
                commands.append(f"BLEDISP {display.upper()}")
        elif step_type == "light_state":
            light = str(step.get("light") or "").lower()
            if light in ("on", "off"):
                commands.append(f"BLELIGHT {light.upper()}")
        elif step_type == "swing_state":
            swing = str(step.get("swing") or "").lower()
            if swing == "horizontal":
                commands.append("BLESWING H")
            elif swing == "vertical":
                commands.append("BLESWING V")
    return commands


@dataclass
class BleCommandResult:
    command: str
    ok: bool
    latency_ms: int
    error: str = ""
    write_ok: bool = False
    released: bool = False


@dataclass
class RoundResult:
    index: int
    text: str
    response: dict[str, Any] = field(default_factory=dict)
    timings: dict[str, int] = field(default_factory=dict)
    ble: list[BleCommandResult] = field(default_factory=list)
    error: str = ""


def execute_ble_commands(
    serial_monitor: SerialMonitor | None,
    commands: Iterable[str],
    timeout_sec: float,
    step_delay_sec: float,
) -> list[BleCommandResult]:
    if serial_monitor is None:
        return []

    results: list[BleCommandResult] = []
    for command in commands:
        serial_monitor.drain_lines()
        start = time.monotonic()
        serial_monitor.send_line(command)
        result = serial_monitor.wait_ble_result(timeout_sec)
        item = BleCommandResult(
            command=command,
            ok=bool(result.get("ok")),
            latency_ms=elapsed_ms(start),
            error=str(result.get("error") or ""),
            write_ok=bool(result.get("write_ok")),
            released=bool(result.get("released")),
        )
        results.append(item)
        on_ble_result(item)
        status = "OK" if item.ok else f"ERR {item.error}"
        info(
            f"[ble] {command}: {status}, latency={item.latency_ms}ms, "
            f"write_ok={item.write_ok}, released={item.released}"
        )
        time.sleep(step_delay_sec)
    return results


# Hook functions. Keep these tiny by default; customize them for one-off diagnostics.
def on_server_log_line(line: str) -> None:
    _ = line


def on_serial_line(line: str) -> None:
    _ = line


def on_server_json(round_result: RoundResult) -> None:
    _ = round_result


def on_ble_result(result: BleCommandResult) -> None:
    _ = result


def on_round_result(round_result: RoundResult) -> None:
    _ = round_result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="EdgeTTS/server/serial BLE aircon probe")
    parser.add_argument("--host", default="127.0.0.1", help="ai_server.py TCP host")
    parser.add_argument("--port", type=int, default=9090, help="ai_server.py TCP port")
    parser.add_argument("--device-id", default="codex_ble_probe", help="fake ESP32 device id for server auth")
    parser.add_argument(
        "--ai-pipeline",
        choices=("split", "omni"),
        default="",
        help="include META.ai_pipeline for servers started with AI_ALLOW_META_PIPELINE_OVERRIDE=true",
    )
    parser.add_argument("--timeout", type=float, default=90.0, help="server socket timeout in seconds")
    parser.add_argument("--text", action="append", default=[], help="utterance to synthesize; can be repeated")
    parser.add_argument("--repeat", type=int, default=1, help="repeat all utterances N times")
    parser.add_argument("--voice", default="zh-CN-XiaoxiaoNeural", help="edge-tts voice")
    parser.add_argument("--rate", default="+0%", help="edge-tts rate, e.g. +0%% or -10%%")
    parser.add_argument("--volume", default="+0%", help="edge-tts volume")
    parser.add_argument("--pitch", default="+0Hz", help="edge-tts pitch")
    parser.add_argument("--audio-file", type=Path, default=None, help="send an existing .pcm or 16k mono .wav")
    parser.add_argument("--server-log", type=Path, default=DEFAULT_SERVER_LOG, help="server log file to tail")
    parser.add_argument("--no-server-log", action="store_true", help="do not tail server log")
    parser.add_argument("--serial-port", default="", help="ESP32 serial port; default auto-detect")
    parser.add_argument("--no-serial", action="store_true", help="do not open serial")
    parser.add_argument("--serial-baud", type=int, default=921600, help="ESP32 serial baud")
    parser.add_argument(
        "--execute-ble-via-serial",
        action="store_true",
        help="map returned control steps to existing BLE serial commands and send them",
    )
    parser.add_argument("--ble-timeout", type=float, default=18.0, help="timeout per BLE serial command")
    parser.add_argument("--ble-step-delay", type=float, default=0.25, help="delay between serial BLE steps")
    parser.add_argument("--diag-before", action="store_true", help="send DIAG before tests")
    parser.add_argument("--diag-after", action="store_true", help="send DIAG after tests")
    parser.add_argument("--json-out", type=Path, default=None, help="write machine-readable summary JSON")
    return parser.parse_args()


def default_texts() -> list[str]:
    return [
        "打开空调",
        "关闭空调",
        "把空调调到二十六度",
        "打开空调，制冷模式，三档风",
    ]


def main() -> int:
    args = parse_args()
    texts = args.text or default_texts()
    serial_monitor: SerialMonitor | None = None
    server_log_path = None if args.no_server_log else args.server_log
    server_log = ServerLogTail(server_log_path, enabled=server_log_path is not None)
    results: list[RoundResult] = []

    try:
        server_log.start()

        if not args.no_serial:
            port = args.serial_port or discover_serial_port()
            if port:
                serial_monitor = SerialMonitor(port, args.serial_baud)
                serial_monitor.start()
                time.sleep(0.3)
            else:
                info("[serial] no serial port found")

        if serial_monitor and args.diag_before:
            serial_monitor.send_line("DIAG")
            time.sleep(2)

        round_index = 0
        for repeat_index in range(args.repeat):
            for text in texts:
                round_index += 1
                round_result = RoundResult(index=round_index, text=text)
                info(f"\n[round {round_index}] repeat={repeat_index + 1}/{args.repeat} text={text}")
                try:
                    if args.audio_file:
                        pcm_data = load_audio_file(args.audio_file)
                        tts_ms = 0
                    else:
                        pcm_data, tts_ms = synthesize_edge_tts_pcm(
                            text, args.voice, args.rate, args.volume, args.pitch
                        )
                    round_result.timings["edge_tts_ms"] = tts_ms
                    round_result.timings["pcm_bytes"] = len(pcm_data)
                    info(f"[round {round_index}] audio pcm={len(pcm_data)}B edge_tts={tts_ms}ms")

                    response, timings = send_audio_to_server(
                        args.host, args.port, args.device_id, pcm_data, args.timeout, args.ai_pipeline
                    )
                    round_result.response = response
                    round_result.timings.update(timings)
                    on_server_json(round_result)

                    control = response.get("control") if isinstance(response, dict) else {}
                    reply = response.get("reply_text", "") if isinstance(response, dict) else ""
                    info(
                        f"[round {round_index}] server json={timings.get('json_ms')}ms "
                        f"eos={timings.get('eos_ms')}ms audio={timings.get('audio_bytes')}B"
                    )
                    info(f"[round {round_index}] reply={reply}")
                    info(f"[round {round_index}] control={json.dumps(control, ensure_ascii=False)}")

                    commands = control_steps_to_serial_commands(control or {})
                    if args.execute_ble_via_serial and commands:
                        if serial_monitor is None:
                            raise RuntimeError("--execute-ble-via-serial requires an open serial port")
                        round_result.ble = execute_ble_commands(
                            serial_monitor,
                            commands,
                            timeout_sec=args.ble_timeout,
                            step_delay_sec=args.ble_step_delay,
                        )
                    elif args.execute_ble_via_serial:
                        info(f"[round {round_index}] no BLE serial commands generated")

                except Exception as exc:
                    round_result.error = str(exc)
                    info(f"[round {round_index}] ERROR: {exc}")
                finally:
                    on_round_result(round_result)
                    results.append(round_result)

        if serial_monitor and args.diag_after:
            serial_monitor.send_line("DIAG")
            time.sleep(2)

    finally:
        if serial_monitor:
            serial_monitor.stop()
        server_log.stop()

    summary = {
        "ok": all(not item.error and all(ble.ok for ble in item.ble) for item in results),
        "host": args.host,
        "port": args.port,
        "ai_pipeline": args.ai_pipeline,
        "rounds": [
            {
                "index": item.index,
                "text": item.text,
                "error": item.error,
                "timings": item.timings,
                "response": item.response,
                "ble": [ble.__dict__ for ble in item.ble],
            }
            for item in results
        ],
    }

    info("\n[summary]")
    for item in results:
        control = item.response.get("control") if isinstance(item.response, dict) else {}
        ble_ok = "n/a" if not item.ble else str(all(ble.ok for ble in item.ble))
        info(
            f"- #{item.index} {item.text}: error={item.error or '-'} "
            f"json={item.timings.get('json_ms', '-')}ms "
            f"rtt={item.timings.get('round_trip_ms', '-')}ms "
            f"has_command={(control or {}).get('has_command', False)} ble_ok={ble_ok}"
        )

    if args.json_out:
        args.json_out.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        info(f"[summary] wrote {args.json_out}")

    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
