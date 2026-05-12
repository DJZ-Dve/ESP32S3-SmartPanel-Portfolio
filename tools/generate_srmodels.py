#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path


DEFAULT_MODELS = (
    "lib/esp-sr/model/wakenet_model/wn9_nihaoxiaoan_tts2",
    "lib/esp-sr/model/vadnet_model/vadnet1_medium",
)


def pack_string(value: str, width: int = 32) -> bytes:
    raw = value.encode("utf-8")
    if len(raw) > width:
        raise ValueError(f"{value} is longer than {width} bytes")
    return raw + b"\0" * (width - len(raw))


def read_model_dir(path: Path) -> tuple[str, list[tuple[str, bytes]]]:
    required = ("_MODEL_INFO_",)
    if not path.is_dir():
        raise FileNotFoundError(f"Model directory not found: {path}")
    for name in required:
        if not (path / name).is_file():
            raise FileNotFoundError(f"Required model file missing: {path / name}")

    files = []
    for child in sorted(path.iterdir(), key=lambda item: item.name):
        if child.is_file():
            files.append((child.name, child.read_bytes()))
    if not files:
        raise ValueError(f"No files found in model directory: {path}")
    return path.name, files


def pack_models(model_dirs: list[Path]) -> bytes:
    models = [read_model_dir(path) for path in model_dirs]
    file_count = sum(len(files) for _, files in models)
    header_len = 4 + len(models) * (32 + 4) + file_count * (32 + 4 + 4)

    header = bytearray()
    payload = bytearray()
    header += struct.pack("<I", len(models))

    for model_name, files in models:
        header += pack_string(model_name)
        header += struct.pack("<I", len(files))
        for file_name, data in files:
            header += pack_string(file_name)
            header += struct.pack("<I", header_len + len(payload))
            header += struct.pack("<I", len(data))
            payload += data

    if len(header) != header_len:
        raise AssertionError(f"Header length mismatch: expected={header_len}, actual={len(header)}")
    return bytes(header + payload)


def main() -> int:
    parser = argparse.ArgumentParser(description="Pack the project ESP-SR WakeNet and VADNet models.")
    parser.add_argument(
        "-o",
        "--output",
        default="board_models/srmodels.bin",
        help="Output srmodels.bin path",
    )
    parser.add_argument(
        "--max-size",
        default="0xA0000",
        help="Maximum allowed image size, matching the model partition",
    )
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    model_dirs = [repo / path for path in DEFAULT_MODELS]
    output = (repo / args.output).resolve() if not Path(args.output).is_absolute() else Path(args.output)
    max_size = int(args.max_size, 0)

    image = pack_models(model_dirs)
    if len(image) > max_size:
        raise SystemExit(f"Model image is too large: {len(image)} bytes > {max_size} bytes")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)

    names = ", ".join(path.name for path in model_dirs)
    print(f"Wrote {output} ({len(image)} bytes, models: {names})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
