#!/usr/bin/env python3
"""Generate a deterministic FLAC malformed corpus.

Native FLAC metadata is a linear block chain, not an arbitrary object graph.
The "cycle" cases therefore use the two places where the format carries
file-relative back references: SEEKTABLE stream offsets and CUESHEET track
offsets.  The generated offsets move backwards or wrap around the stream.
"""

import argparse
import csv
import os
import random
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path


FLAC_MARKER = b"fLaC"
CATEGORIES = ("truncate", "illegal_metadata", "oversized_block", "cycle_reference")


@dataclass
class Block:
    type: int
    body: bytes
    declared_length: int | None = None


def parse_metadata(data: bytes):
    if not data.startswith(FLAC_MARKER):
        raise ValueError("seed is not a native FLAC stream")
    blocks = []
    offset = 4
    while True:
        if offset + 4 > len(data):
            raise ValueError("truncated seed metadata header")
        header = data[offset : offset + 4]
        is_last = bool(header[0] & 0x80)
        block_type = header[0] & 0x7F
        length = int.from_bytes(header[1:4], "big")
        body_offset = offset + 4
        body_end = body_offset + length
        if body_end > len(data):
            raise ValueError("truncated seed metadata body")
        blocks.append(Block(block_type, data[body_offset:body_end]))
        offset = body_end
        if is_last:
            break
    if not blocks or blocks[0].type != 0 or len(blocks[0].body) != 34:
        raise ValueError("seed does not start with a 34-byte STREAMINFO")
    return blocks, data[offset:]


def serialize(blocks, audio: bytes) -> bytes:
    out = bytearray(FLAC_MARKER)
    for index, block in enumerate(blocks):
        length = len(block.body) if block.declared_length is None else block.declared_length
        if length > 0xFFFFFF:
            raise ValueError("metadata block is too large for its 24-bit length")
        last = 0x80 if index == len(blocks) - 1 else 0
        out.append(last | (block.type & 0x7F))
        out.extend(length.to_bytes(3, "big"))
        out.extend(block.body)
    out.extend(audio)
    return bytes(out)


def insert_block(blocks, block, rng, first=False):
    index = 1 if first else rng.randint(1, len(blocks))
    blocks.insert(index, block)
    return 4 if index == 0 else 4 + sum(4 + len(item.body) for item in blocks[:index])


def make_seektable(points):
    body = bytearray()
    for sample_number, stream_offset, frame_samples in points:
        body.extend(struct.pack(">QQH", sample_number, stream_offset, frame_samples))
    return Block(3, bytes(body))


def make_cuesheet(track_offsets, track_numbers, lead_out_offset=0):
    body = bytearray(128)
    body.extend(struct.pack(">Q", 88200))
    body.append(0)
    body.extend(b"\x00" * 258)
    body.append(len(track_offsets) + 1)
    for index, offset in enumerate(track_offsets):
        body.extend(struct.pack(">Q", offset & 0xFFFFFFFFFFFFFFFF))
        body.append(track_numbers[index])
        body.extend(b"\x00" * 12)
        body.append(0)
        body.extend(b"\x00" * 13)
        body.append(1)
        body.extend(struct.pack(">Q", 0))
        body.append(1)
        body.extend(b"\x00" * 3)
    body.extend(struct.pack(">Q", lead_out_offset))
    body.append(170)
    body.extend(b"\x00" * 12)
    body.append(0)
    body.extend(b"\x00" * 13)
    body.append(0)
    assert len(body) == 432 + 48 * len(track_offsets)
    return Block(5, bytes(body))


def malformed_vorbis(vendor_length=None, comment_count=None):
    vendor = b"property-test"
    body = bytearray()
    body.extend(struct.pack("<I", vendor_length if vendor_length is not None else len(vendor)))
    body.extend(vendor)
    body.extend(struct.pack("<I", comment_count if comment_count is not None else 1))
    body.extend(struct.pack("<I", 3))
    body.extend(b"k=v")
    return Block(4, bytes(body))


def malformed_picture():
    body = bytearray()
    body.extend(struct.pack(">I", 3))
    body.extend(b"img")
    body.extend(struct.pack(">I", 0xFFFFFFFF))
    body.extend(b"x")
    body.extend(struct.pack(">IIII", 1, 1, 24, 0))
    body.extend(struct.pack(">I", 4))
    body.extend(b"data")
    return Block(6, bytes(body))


def patch_streaminfo(block, variant):
    body = bytearray(block.body)
    if variant == "rate0":
        packed = int.from_bytes(body[10:18], "big")
        packed &= (1 << 44) - 1
        body[10:18] = packed.to_bytes(8, "big")
    elif variant == "bps1":
        packed = int.from_bytes(body[10:18], "big")
        packed &= ~(((1 << 5) - 1) << 36)
        body[10:18] = packed.to_bytes(8, "big")
    elif variant == "minblock0":
        body[0:2] = b"\x00\x00"
    elif variant == "framesize_inverted":
        body[4:7] = b"\xff\xff\xff"
        body[7:10] = b"\x00\x00\x01"
    elif variant == "total_samples_max":
        packed = int.from_bytes(body[10:18], "big")
        packed |= (1 << 36) - 1
        body[10:18] = packed.to_bytes(8, "big")
    elif variant == "short":
        body = body[:20]
    else:
        raise ValueError(variant)
    return Block(0, bytes(body))


def make_truncate(data, blocks, audio, rng, index):
    metadata_end = len(data) - len(audio)
    variants = (
        "metadata_header",
        "metadata_body",
        "audio_mid",
        "audio_tail",
        "one_byte_short",
    )
    variant = variants[index % len(variants)]
    if variant == "metadata_header":
        cut = rng.randint(4, max(5, metadata_end))
    elif variant == "metadata_body":
        cut = rng.randint(5, max(6, metadata_end))
    elif variant == "one_byte_short":
        cut = len(data) - 1
    elif variant == "audio_tail":
        cut = len(data) - rng.randint(2, max(3, min(64, len(audio))))
    else:
        cut = metadata_end + rng.randrange(max(1, len(audio)))
    cut = min(max(cut, 1), len(data) - 1)
    return data[:cut], cut, variant


def make_illegal(blocks, audio, index):
    variants = (
        "reserved_type_127",
        "application_short",
        "duplicate_streaminfo",
        "streaminfo_rate0",
        "streaminfo_bps1",
        "streaminfo_minblock0",
        "streaminfo_framesize_inverted",
        "streaminfo_total_samples_max",
        "vorbis_vendor_length",
        "vorbis_comment_count",
        "picture_mime_length",
        "seektable_bad_length",
        "cuesheet_short",
    )
    variant = variants[index % len(variants)]
    blocks = [Block(item.type, item.body, item.declared_length) for item in blocks]
    if variant == "reserved_type_127":
        offset = insert_block(blocks, Block(127, b"reserved"), random.Random(index))
    elif variant == "application_short":
        offset = insert_block(blocks, Block(2, b"abc"), random.Random(index + 1))
    elif variant == "duplicate_streaminfo":
        blocks.insert(1, Block(0, blocks[0].body))
        offset = 4 + 4 + len(blocks[0].body)
    elif variant.startswith("streaminfo_"):
        blocks[0] = patch_streaminfo(blocks[0], variant.removeprefix("streaminfo_"))
        offset = 8
    elif variant == "vorbis_vendor_length":
        offset = insert_block(blocks, malformed_vorbis(vendor_length=0xFFFFFF00), random.Random(index))
    elif variant == "vorbis_comment_count":
        offset = insert_block(blocks, malformed_vorbis(comment_count=0xFFFFFFFF), random.Random(index))
    elif variant == "picture_mime_length":
        offset = insert_block(blocks, malformed_picture(), random.Random(index))
    elif variant == "seektable_bad_length":
        offset = insert_block(blocks, Block(3, b"\x00" * 19), random.Random(index))
    elif variant == "cuesheet_short":
        offset = insert_block(blocks, Block(5, b"\x00" * 20), random.Random(index))
    else:
        raise ValueError(variant)
    return serialize(blocks, audio), offset, variant


def make_oversized(blocks, audio, index):
    variants = (
        "padding_declared_max",
        "application_declared_max",
        "seektable_declared_max",
        "header_then_eof",
        "streaminfo_frame_max",
    )
    variant = variants[index % len(variants)]
    blocks = [Block(item.type, item.body, item.declared_length) for item in blocks]
    rng = random.Random(1000 + index)
    if variant == "padding_declared_max":
        return (
            serialize(blocks + [Block(1, b"\x00" * 64, 0xFFFFFF)], audio),
            len(serialize(blocks, b"")),
            variant,
        )
    if variant == "application_declared_max":
        return (
            serialize(blocks + [Block(2, b"OVR!" + b"\x00" * 32, 0xFFFFFF)], audio),
            len(serialize(blocks, b"")),
            variant,
        )
    if variant == "seektable_declared_max":
        return (
            serialize(blocks + [Block(3, b"\x00" * 18, 0xFFFFFF)], audio),
            len(serialize(blocks, b"")),
            variant,
        )
    if variant == "header_then_eof":
        header = bytes([0x80 | rng.choice((1, 2, 3, 4, 5, 6))]) + b"\xff\xff\xff"
        prefix = serialize(blocks, b"")
        return prefix + header, len(prefix), variant
    blocks[0] = patch_streaminfo(blocks[0], "framesize_inverted")
    return serialize(blocks, audio), 8, variant


def make_cycle(blocks, audio, index):
    variants = (
        "seektable_backward",
        "seektable_wrap",
        "seektable_duplicate",
        "cuesheet_descending",
        "cuesheet_duplicate_track",
        "cuesheet_leadout_early",
    )
    variant = variants[index % len(variants)]
    blocks = [Block(item.type, item.body, item.declared_length) for item in blocks]
    rng = random.Random(2000 + index)
    if variant == "seektable_backward":
        block = make_seektable(
            [
                (100, 0xFFFFFFFFFFFFFFFF, 4096),
                (50, 0, 4096),
                (0, 0xFFFFFFFFFFFFFF00, 4096),
                (0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0),
            ]
        )
    elif variant == "seektable_wrap":
        block = make_seektable(
            [
                (0, 0xFFFFFFFFFFFFFFF0, 4096),
                (1, 1, 4096),
                (2, 0xFFFFFFFFFFFFFFF0, 4096),
            ]
        )
    elif variant == "seektable_duplicate":
        block = make_seektable([(8, 0, 4096), (0, 64, 4096), (8, 0, 4096)])
        blocks.append(Block(3, block.body))
    elif variant == "cuesheet_descending":
        block = make_cuesheet([50000, 1000, 250], [1, 2, 3], 0)
    elif variant == "cuesheet_duplicate_track":
        block = make_cuesheet([0, 1000, 2000], [1, 1, 170], 3000)
    elif variant == "cuesheet_leadout_early":
        block = make_cuesheet([5000, 6000], [1, 2], 0)
    else:
        raise ValueError(variant)
    offset = insert_block(blocks, block, rng)
    return serialize(blocks, audio), offset, variant


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--count", type=int, default=200)
    parser.add_argument("--seed-value", type=int, default=4017)
    args = parser.parse_args()

    if args.count < 0:
        parser.error("--count must be non-negative")
    data = args.seed.read_bytes()
    blocks, audio = parse_metadata(data)
    if not audio:
        raise SystemExit("seed contains no audio frames")

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    manifest_path = args.out / "manifest.tsv"

    counters = {category: 0 for category in CATEGORIES}
    rows = []
    for index in range(args.count):
        category = CATEGORIES[index % len(CATEGORIES)]
        category_index = counters[category]
        counters[category] += 1
        rng = random.Random(args.seed_value * 100003 + index)

        if category == "truncate":
            blob, injected_offset, variant = make_truncate(data, blocks, audio, rng, category_index)
        elif category == "illegal_metadata":
            blob, injected_offset, variant = make_illegal(blocks, audio, category_index)
        elif category == "oversized_block":
            blob, injected_offset, variant = make_oversized(blocks, audio, category_index)
        else:
            blob, injected_offset, variant = make_cycle(blocks, audio, category_index)

        filename = f"{index:03d}_{category}_{variant}.flac"
        path = args.out / filename
        path.write_bytes(blob)
        rows.append(
            {
                "id": index,
                "category": category,
                "variant": variant,
                "path": path,
                "size": len(blob),
                "injected_offset": injected_offset,
            }
        )

    with manifest_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=("id", "category", "variant", "path", "size", "injected_offset"),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    print(
        f"generated {len(rows)} cases "
        + " ".join(f"{category}={counters[category]}" for category in CATEGORIES)
    )
    print(manifest_path)


if __name__ == "__main__":
    main()
