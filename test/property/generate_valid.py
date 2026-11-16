#!/usr/bin/env python3
"""Generate valid, deterministic FLAC files with ffmpeg for differential tests."""

import argparse
import csv
import json
import shutil
import subprocess
from pathlib import Path


RATES = (8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000)
DURATIONS = (0.08, 0.12, 0.20, 0.35, 0.50)


def probe(ffprobe: str, path: Path):
    command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "a:0",
        "-show_entries",
        "stream=channels,sample_rate,bits_per_raw_sample,duration_ts",
        "-of",
        "json",
        str(path),
    ]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    return json.loads(result.stdout)["streams"][0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--count", type=int, default=50)
    args = parser.parse_args()

    if args.count < 0:
        parser.error("--count must be non-negative")
    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    rows = []
    for index in range(args.count):
        rate = RATES[index % len(RATES)]
        channels = 1 if index % 2 == 0 else 2
        bits_per_sample = 16 if (index // 2) % 2 == 0 else 24
        duration = DURATIONS[(index // 4) % len(DURATIONS)]
        frequency = 220.0 + 37.0 * (index % 17)
        compression_level = index % 9
        ch_mode = ("auto", "indep", "left_side", "right_side", "mid_side")[index % 5]
        path = args.out / f"{index:03d}.flac"

        left = (
            f"0.32*sin(2*PI*{frequency:.3f}*t)"
            f"+0.04*sin(2*PI*{frequency * 2.5:.3f}*t)"
        )
        if channels == 1:
            source = (
                f"aevalsrc={left}:s={rate}:d={duration:.3f}:c=mono"
            )
        else:
            right = (
                f"0.27*sin(2*PI*{frequency * 1.5:.3f}*t)"
                f"+0.03*sin(2*PI*{frequency * 3.25:.3f}*t)"
            )
            source = (
                f"aevalsrc={left}|{right}:s={rate}:d={duration:.3f}:c=stereo"
            )

        command = [
            args.ffmpeg,
            "-v",
            "error",
            "-nostdin",
            "-y",
            "-f",
            "lavfi",
            "-i",
            source,
            "-map",
            "0:a:0",
            "-c:a",
            "flac",
            "-compression_level",
            str(compression_level),
            "-ch_mode",
            ch_mode,
            "-sample_fmt",
            "s16" if bits_per_sample == 16 else "s32",
            "-fflags",
            "+bitexact",
            "-flags:a",
            "+bitexact",
            str(path),
        ]
        subprocess.run(command, check=True)
        info = probe(args.ffprobe, path)
        if int(info["channels"]) != channels or int(info["sample_rate"]) != rate:
            raise SystemExit(f"unexpected stream format in {path}: {info}")
        if int(info.get("bits_per_raw_sample", 0)) != bits_per_sample:
            raise SystemExit(f"unexpected bit depth in {path}: {info}")

        rows.append(
            {
                "id": index,
                "path": path,
                "channels": channels,
                "bps": bits_per_sample,
                "rate": rate,
                "samples": int(info["duration_ts"]),
                "compression_level": compression_level,
                "ch_mode": ch_mode,
            }
        )

    manifest = args.out / "manifest.tsv"
    with manifest.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=(
                "id",
                "path",
                "channels",
                "bps",
                "rate",
                "samples",
                "compression_level",
                "ch_mode",
            ),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)
    print(f"generated {len(rows)} valid FLAC files")
    print(manifest)


if __name__ == "__main__":
    main()
