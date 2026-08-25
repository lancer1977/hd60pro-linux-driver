#!/usr/bin/env python3
"""Regenerate the host H.264 placeholder from the card's vendor splash.

The original 320x240 luma asset is NOSG_LOGO_Y in the locally extracted
tinyvenc5 ELF.  It is not copied into this script: extraction is accepted only
when its SHA-256 matches the asset identified byte-for-byte in hardware capture
M127b.  The canvas matches that capture exactly (Y=0x11, U=V=0x80), then ffmpeg
encodes one self-contained 1920x1080 High-profile Annex-B access unit.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import tempfile
from pathlib import Path


WIDTH, HEIGHT = 1920, 1080
LOGO_WIDTH, LOGO_HEIGHT = 320, 240
LOGO_X, LOGO_Y = (WIDTH - LOGO_WIDTH) // 2, (HEIGHT - LOGO_HEIGHT) // 2
LOGO_FILE_OFFSET = 0x5B10C
LOGO_LENGTH = LOGO_WIDTH * LOGO_HEIGHT
LOGO_SHA256 = "dfce4efd5139298f544d23473f85a42fb7115a3c5e4ba65b71c070d59883a30b"


def extract_logo(path: Path) -> bytes:
    image = path.read_bytes()
    logo = image[LOGO_FILE_OFFSET:LOGO_FILE_OFFSET + LOGO_LENGTH]
    digest = hashlib.sha256(logo).hexdigest()
    if len(logo) != LOGO_LENGTH or digest != LOGO_SHA256:
        raise SystemExit(
            f"{path}: NOSG_LOGO_Y identity mismatch "
            f"(length={len(logo)}, sha256={digest})"
        )
    return logo


def make_i420_frame(logo: bytes) -> bytes:
    y = bytearray([0x11]) * (WIDTH * HEIGHT)
    for row in range(LOGO_HEIGHT):
        dst = (LOGO_Y + row) * WIDTH + LOGO_X
        src = row * LOGO_WIDTH
        y[dst:dst + LOGO_WIDTH] = logo[src:src + LOGO_WIDTH]
    chroma = bytes([0x80]) * (WIDTH * HEIGHT // 2)
    return bytes(y) + chroma


def nal_types(data: bytes) -> list[int]:
    types: list[int] = []
    i = 0
    while i + 4 < len(data):
        if data[i:i + 4] == b"\x00\x00\x00\x01":
            start = i + 4
        elif data[i:i + 3] == b"\x00\x00\x01":
            start = i + 3
        else:
            i += 1
            continue
        types.append(data[start] & 0x1F)
        i = start + 1
    return types


def encode(frame: bytes) -> bytes:
    with tempfile.TemporaryDirectory(prefix="mz0380-nosg-") as directory:
        raw = Path(directory) / "vendor-no-signal.i420"
        encoded = Path(directory) / "vendor-no-signal.h264"
        raw.write_bytes(frame)
        subprocess.run(
            [
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                "-f", "rawvideo", "-pixel_format", "yuv420p",
                "-video_size", f"{WIDTH}x{HEIGHT}", "-framerate", "2",
                "-i", str(raw), "-frames:v", "1", "-an",
                "-c:v", "libx264", "-preset", "slow", "-tune", "stillimage",
                "-profile:v", "high", "-level:v", "4.2", "-qp", "12",
                "-x264-params",
                "keyint=1:min-keyint=1:scenecut=0:bframes=0:aud=1:repeat-headers=1",
                "-pix_fmt", "yuv420p", "-f", "h264", str(encoded),
            ],
            check=True,
        )
        data = encoded.read_bytes()
    types = nal_types(data)
    required = {9, 7, 8, 5}  # AUD, SPS, PPS, IDR
    if not required.issubset(types):
        raise SystemExit(f"encoded access unit has NAL types {types}, need {required}")
    return data


def write_header(path: Path, data: bytes) -> None:
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-or-later */",
        "/* Generated from tinyvenc5 NOSG_LOGO_Y; see mz0380-no-signal-generate.py. */",
        "static const unsigned char mz0380_no_signal_h264[] = {",
    ]
    for offset in range(0, len(data), 12):
        chunk = data[offset:offset + 12]
        lines.append("  " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines.extend(
        [
            "};",
            "static const size_t mz0380_no_signal_h264_len =",
            "\tsizeof(mz0380_no_signal_h264);",
            "",
        ]
    )
    path.write_text("\n".join(lines))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--tinyvenc5",
        type=Path,
        default=Path("re-dump/fw/yuan_demo_sdi/tinyvenc5"),
    )
    parser.add_argument(
        "--output", type=Path, default=Path("mz0380-no-signal-data.h")
    )
    parser.add_argument("--h264-output", type=Path)
    parser.add_argument("--raw-output", type=Path)
    args = parser.parse_args()

    logo = extract_logo(args.tinyvenc5)
    frame = make_i420_frame(logo)
    encoded = encode(frame)
    write_header(args.output, encoded)
    if args.h264_output:
        args.h264_output.write_bytes(encoded)
    if args.raw_output:
        args.raw_output.write_bytes(frame)
    print(
        f"vendor logo sha256={LOGO_SHA256}; wrote {len(encoded)}-byte "
        f"AUD/SPS/PPS/IDR to {args.output}"
    )


if __name__ == "__main__":
    main()
