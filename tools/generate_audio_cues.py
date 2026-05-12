#!/usr/bin/env python3
"""
Generate short UI/audio cue tones for the ESP32-S3 firmware.

Outputs:
  - raw signed 16-bit little-endian PCM: 16 kHz, mono, no WAV header
  - optional WAV preview files with the same samples

Examples:
  python tools/generate_audio_cues.py
  python tools/generate_audio_cues.py --header include/audio_cues_data.h
  python tools/generate_audio_cues.py --out asset/audio_cues --no-wav
  python tools/generate_audio_cues.py --volume 0.55 --only boot,low_battery
"""

from __future__ import annotations

import argparse
import json
import math
import random
import struct
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

SAMPLE_RATE = 16_000
PCM_MAX = 32767


@dataclass(frozen=True)
class Segment:
    duration_ms: int
    start_freq: float = 0.0
    end_freq: float | None = None
    gain: float = 1.0
    kind: str = "tone"
    attack_ms: int = 7
    release_ms: int = 24


@dataclass(frozen=True)
class Cue:
    name: str
    description: str
    priority: str
    segments: tuple[Segment, ...]
    gap_ms: int = 0
    lead_ms: int = 0


CUES: tuple[Cue, ...] = (
    Cue(
        name="boot",
        description="audio subsystem ready",
        priority="high",
        segments=(Segment(70, 523, 659, 0.42), Segment(88, 659, 880, 0.46)),
        gap_ms=14,
    ),
    Cue(
        name="low_battery",
        description="low battery warning",
        priority="high",
        segments=(Segment(118, 440, 392, 0.42), Segment(118, 392, 349, 0.42), Segment(118, 349, 330, 0.40)),
        gap_ms=42,
    ),
    Cue(
        name="record_stop",
        description="recording stopped",
        priority="high",
        segments=(
            Segment(80, 1047, 1047, 0.48, attack_ms=4, release_ms=18),
            Segment(80, 1175, 1175, 0.48, attack_ms=4, release_ms=18),
        ),
        gap_ms=140,
        lead_ms=70,
    ),
    Cue(
        # 学习场景捕捉阶段进入提示：低-高双音，让用户知道"现在按遥控器"。
        name="learn_capture",
        description="learn flow: capturing remote button",
        priority="high",
        segments=(
            Segment(70, 523, 523, 0.46, attack_ms=4, release_ms=20),
            Segment(110, 784, 988, 0.50, attack_ms=4, release_ms=22),
        ),
        gap_ms=18,
        lead_ms=40,
    ),
    Cue(
        # 学习场景标签阶段进入提示：上升三音，让用户知道"现在说话起标签"。
        name="learn_label",
        description="learn flow: awaiting voice label",
        priority="high",
        segments=(
            Segment(70, 659, 784, 0.46, attack_ms=4, release_ms=18),
            Segment(70, 880, 988, 0.48, attack_ms=4, release_ms=18),
            Segment(110, 1175, 1319, 0.50, attack_ms=4, release_ms=22),
        ),
        gap_ms=12,
        lead_ms=40,
    ),
)


def smooth_step(x: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    return x * x * (3.0 - 2.0 * x)


def envelope(index: int, total: int, attack_ms: int = 7, release_ms: int = 24) -> float:
    if total <= 1:
        return 1.0
    attack = max(1, SAMPLE_RATE * attack_ms // 1000)
    release = max(1, SAMPLE_RATE * release_ms // 1000)
    value = 1.0
    if index < attack:
        value *= smooth_step(index / attack)
    release_start = total - release
    if index >= release_start:
        value *= smooth_step((total - 1 - index) / release)
    return value


def tone_segment(segment: Segment) -> list[float]:
    total = max(1, SAMPLE_RATE * segment.duration_ms // 1000)
    end_freq = segment.start_freq if segment.end_freq is None else segment.end_freq
    phase = 0.0
    samples: list[float] = []
    for index in range(total):
        t = index / (total - 1) if total > 1 else 1.0
        glide = smooth_step(t)
        freq = segment.start_freq + (end_freq - segment.start_freq) * glide
        phase += 2.0 * math.pi * freq / SAMPLE_RATE
        harmonic = (
            0.86 * math.sin(phase)
            + 0.10 * math.sin(phase * 2.0)
            + 0.04 * math.sin(phase * 3.0)
        )
        samples.append(segment.gain * envelope(index, total, segment.attack_ms, segment.release_ms) * harmonic)
    return samples


def noise_tick_segment(segment: Segment, rng: random.Random) -> list[float]:
    total = max(1, SAMPLE_RATE * segment.duration_ms // 1000)
    samples: list[float] = []
    for index in range(total):
        decay = math.exp(-8.0 * index / total)
        samples.append(segment.gain * envelope(index, total, 1, 16) * decay * rng.uniform(-1.0, 1.0))
    return samples


def render_segment(segment: Segment, rng: random.Random) -> list[float]:
    if segment.kind == "noise_tick":
        return noise_tick_segment(segment, rng)
    return tone_segment(segment)


def render_cue(cue: Cue, volume: float, seed: int) -> list[int]:
    rng = random.Random(seed)
    float_samples: list[float] = []
    lead_samples = max(0, SAMPLE_RATE * cue.lead_ms // 1000)
    gap_samples = max(0, SAMPLE_RATE * cue.gap_ms // 1000)

    if lead_samples:
        float_samples.extend([0.0] * lead_samples)

    for index, segment in enumerate(cue.segments):
        if index > 0 and gap_samples:
            float_samples.extend([0.0] * gap_samples)
        float_samples.extend(render_segment(segment, rng))

    peak = max((abs(sample) for sample in float_samples), default=1.0)
    limiter = min(1.0, 0.92 / peak) if peak > 0 else 1.0

    pcm: list[int] = []
    for sample in float_samples:
        shaped = math.tanh(sample * limiter * volume * 1.35)
        pcm.append(max(-PCM_MAX, min(PCM_MAX, int(shaped * PCM_MAX))))
    return pcm


def write_pcm(path: Path, samples: Iterable[int]) -> int:
    count = 0
    with path.open("wb") as handle:
        for sample in samples:
            handle.write(struct.pack("<h", sample))
            count += 1
    return count


def write_wav(path: Path, samples: Iterable[int]) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        for sample in samples:
            wav.writeframesraw(struct.pack("<h", sample))


def c_symbol(name: str) -> str:
    result = []
    for ch in name:
        if ch.isalnum():
            result.append(ch.lower())
        else:
            result.append("_")
    return "".join(result)


def write_header(path: Path, rendered: list[tuple[Cue, list[int]]]) -> None:
    lines: list[str] = [
        "#pragma once",
        "",
        "#if __has_include(<Arduino.h>)",
        "#include <Arduino.h>",
        "#else",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#ifndef PROGMEM",
        "#define PROGMEM",
        "#endif",
        "#endif",
        "",
        "// Generated by tools/generate_audio_cues.py.",
        "// Format: raw PCM, signed 16-bit little-endian, mono, 16000 Hz.",
        "",
        "struct AudioCueAsset {",
        "    const char* name;",
        "    const uint8_t* data;",
        "    size_t len;",
        "    uint16_t durationMs;",
        "};",
        "",
    ]

    for cue, samples in rendered:
        symbol = f"g_audio_cue_{c_symbol(cue.name)}"
        lines.append(f"static const uint8_t {symbol}[] PROGMEM = {{")
        raw = b"".join(struct.pack("<h", sample) for sample in samples)
        for offset in range(0, len(raw), 16):
            chunk = raw[offset : offset + 16]
            values = ", ".join(f"0x{byte:02X}" for byte in chunk)
            lines.append(f"    {values},")
        lines.append("};")
        lines.append("")

    lines.append("static const AudioCueAsset g_audio_cue_assets[] = {")
    for cue, samples in rendered:
        symbol = f"g_audio_cue_{c_symbol(cue.name)}"
        duration_ms = round(len(samples) * 1000 / SAMPLE_RATE)
        lines.append(f'    {{"{cue.name}", {symbol}, sizeof({symbol}), {duration_ms}}},')
    lines.append("};")
    lines.append("")
    lines.append("static constexpr size_t kAudioCueAssetCount = sizeof(g_audio_cue_assets) / sizeof(g_audio_cue_assets[0]);")
    lines.append("")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_only(value: str) -> set[str] | None:
    if not value:
        return None
    names = {item.strip() for item in value.split(",") if item.strip()}
    return names or None


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Generate 16 kHz mono PCM UI cue sounds.")
    parser.add_argument("--out", type=Path, default=Path("asset/audio_cues"), help="Output directory")
    parser.add_argument("--volume", type=float, default=0.72, help="Master volume, 0.0 to 1.0")
    parser.add_argument("--seed", type=int, default=20260429, help="Deterministic seed for generated noise")
    parser.add_argument("--only", default="", help="Comma-separated cue names to generate")
    parser.add_argument("--no-wav", action="store_true", help="Do not write WAV preview files")
    parser.add_argument("--manifest", default="manifest.json", help="Manifest filename, empty to skip")
    parser.add_argument("--header", type=Path, default=None, help="Optional generated C++ header path")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    selected = parse_only(args.only)
    known = {cue.name for cue in CUES}
    if selected:
        unknown = selected - known
        if unknown:
            print(f"unknown cue name(s): {', '.join(sorted(unknown))}")
            print(f"known cue names: {', '.join(sorted(known))}")
            return 2

    volume = max(0.0, min(1.0, args.volume))
    args.out.mkdir(parents=True, exist_ok=True)
    wav_dir = args.out / "preview_wav"
    if not args.no_wav:
        wav_dir.mkdir(parents=True, exist_ok=True)

    manifest = {
        "format": "raw PCM, signed 16-bit little-endian, mono, no WAV header",
        "sample_rate_hz": SAMPLE_RATE,
        "volume": volume,
        "cues": [],
    }

    rendered: list[tuple[Cue, list[int]]] = []

    for cue in CUES:
        if selected and cue.name not in selected:
            continue
        samples = render_cue(cue, volume=volume, seed=args.seed + len(cue.name))
        rendered.append((cue, samples))
        pcm_path = args.out / f"{cue.name}.pcm"
        sample_count = write_pcm(pcm_path, samples)
        if not args.no_wav:
            write_wav(wav_dir / f"{cue.name}.wav", samples)

        duration_ms = round(sample_count * 1000 / SAMPLE_RATE)
        manifest["cues"].append(
            {
                "name": cue.name,
                "description": cue.description,
                "priority": cue.priority,
                "pcm_file": pcm_path.name,
                "duration_ms": duration_ms,
                "bytes": sample_count * 2,
            }
        )
        print(f"{cue.name:14s} {duration_ms:4d} ms  {sample_count * 2:5d} bytes")

    if args.manifest:
        manifest_path = args.out / args.manifest
        manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"manifest: {manifest_path}")

    if args.header:
        write_header(args.header, rendered)
        print(f"header: {args.header}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
