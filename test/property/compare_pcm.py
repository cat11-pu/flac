#!/usr/bin/env python3
"""Compare two interleaved little-endian s32 PCM files sample by sample."""

import argparse
import array
import json
import sys


def load_s32(path):
    values = array.array("i")
    with open(path, "rb") as handle:
        values.frombytes(handle.read())
    if values.itemsize != 4:
        raise SystemExit("host int is not 32 bits")
    if sys.byteorder != "little":
        values.byteswap()
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lib", required=True)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--channels", required=True, type=int)
    parser.add_argument("--bps", required=True, type=int)
    args = parser.parse_args()

    if args.channels < 1:
        parser.error("--channels must be positive")
    lib = load_s32(args.lib)
    ref = load_s32(args.ref)
    common = min(len(lib), len(ref))
    mismatch = -1
    for index in range(common):
        if lib[index] != ref[index]:
            mismatch = index
            break

    if mismatch < 0 and len(lib) == len(ref):
        print(
            json.dumps(
                {
                    "match": True,
                    "samples_compared": len(lib),
                    "frames_compared": len(lib) // args.channels,
                    "lib_samples": len(lib),
                    "ref_samples": len(ref),
                    "bps": args.bps,
                },
                separators=(",", ":"),
            )
        )
        return 0

    if mismatch < 0:
        mismatch = common
    channel = mismatch % args.channels + 1
    frame = mismatch // args.channels
    print(
        json.dumps(
            {
                "match": False,
                "sample_index": mismatch,
                "frame": frame,
                "channel": channel,
                "byte_offset": mismatch * 4,
                "lib_samples": len(lib),
                "ref_samples": len(ref),
                "lib_value": lib[mismatch] if mismatch < len(lib) else None,
                "ref_value": ref[mismatch] if mismatch < len(ref) else None,
                "bps": args.bps,
            },
            separators=(",", ":"),
        )
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
