#!/usr/bin/env python3
"""Generate malformed FLAC files from valid seeds.

Categories:
  trunc   - truncated files (truncated frames / metadata)
  metahdr - illegal metadata block headers (bad type, huge/zero length)
  frames  - corrupted frame headers (huge blocksize, bad reserved bits)
  flips   - random byte flips anywhere
"""
import os, random, struct, sys

random.seed(1234)
SRC = "fuzz/corpus"
DST = "fuzz/malformed"
os.makedirs(DST, exist_ok=True)
count = 0

def emit(name, data):
    global count
    with open(os.path.join(DST, name), "wb") as f:
        f.write(data)
    count += 1

def meta_blocks(data):
    """Parse metadata block headers; return list of (hdr_off, type, length, is_last)."""
    blocks = []
    if data[:4] != b"fLaC":
        return blocks
    off = 4
    while off + 4 <= len(data):
        t = data[off] & 0x7F
        last = bool(data[off] & 0x80)
        ln = int.from_bytes(data[off+1:off+4], "big")
        blocks.append((off, t, ln, last))
        off += 4 + ln
        if last:
            break
    return blocks

for fn in sorted(os.listdir(SRC)):
    if not fn.endswith(".flac"):
        continue
    base = fn[:-5]
    data = open(os.path.join(SRC, fn), "rb").read()
    n = len(data)
    blocks = meta_blocks(data)
    audio_start = (blocks[-1][0] + 4 + blocks[-1][2]) if blocks else 4

    # 1. truncations: dense near metadata end + spread through stream
    points = set()
    for b in blocks:
        for d in range(-2, 6):
            p = b[0] + d
            if 0 < p < n: points.add(p)
        p = b[0] + 4 + b[2] - 1
        if 0 < p < n: points.add(p)
    for i in range(1, 40):
        points.add(audio_start + (n - audio_start) * i // 40)
    points.add(n - 1); points.add(n - 2); points.add(n // 2)
    for p in sorted(points):
        if 0 < p < n:
            emit(f"{base}.trunc.{p:06d}.flac", data[:p])

    # 2. illegal metadata block headers
    for (off, t, ln, last) in blocks:
        for bad_t in (7, 8, 100, 126, 127):  # reserved/invalid types
            m = bytearray(data); m[off] = (m[off] & 0x80) | bad_t
            emit(f"{base}.metahdr.type{bad_t}@{off}.flac", bytes(m))
        for bad_len in (0xFFFFFF, 0xFFFFF0, n, n - off, 0x800000):
            m = bytearray(data)
            m[off+1:off+4] = bad_len.to_bytes(3, "big")
            emit(f"{base}.metahdr.len{bad_len}@{off}.flac", bytes(m))
        if t == 0:  # STREAMINFO with wrong lengths
            for bad_len in (0, 1, 33, 35, 100):
                m = bytearray(data)
                m[off+1:off+4] = bad_len.to_bytes(3, "big")
                emit(f"{base}.metahdr.si_len{bad_len}@{off}.flac", bytes(m))

    # 3. corrupt STREAMINFO contents (min/max blocksize & framesize combos)
    si = blocks[0][0] + 4 if blocks else None
    if si is not None:
        for vals in ((0,0), (65535,65535), (65535,16), (0,65535), (15,15), (1,1)):
            m = bytearray(data)
            m[si:si+2] = vals[0].to_bytes(2, "big")   # min blocksize
            m[si+2:si+4] = vals[1].to_bytes(2, "big") # max blocksize
            emit(f"{base}.si.bs{vals[0]}_{vals[1]}.flac", bytes(m))
        # min/max framesize huge
        m = bytearray(data); m[si+4:si+7] = b"\xff\xff\xff"; m[si+7:si+10] = b"\xff\xff\xff"
        emit(f"{base}.si.fsmax.flac", bytes(m))

    # 4. corrupt frame headers: scan for 0xFFF8 sync in audio region
    syncs = []
    i = audio_start
    while i + 2 < n and len(syncs) < 12:
        if data[i] == 0xFF and (data[i+1] & 0xFC) == 0xF8:
            syncs.append(i)
            i += max(1, (n - audio_start) // 12)
        else:
            i += 1
    for s in syncs:
        for bs_code in (0x0, 0xF, 0x6, 0x7):  # reserved/unusual blocksize codes
            m = bytearray(data)
            m[s+2] = (bs_code << 4) | (m[s+2] & 0x0F)
            emit(f"{base}.frame.bs{bs_code:x}@{s}.flac", bytes(m))
        m = bytearray(data); m[s+3] = 0xFF  # max channel assignment + bps/reserved bits
        emit(f"{base}.frame.ch15@{s}.flac", bytes(m))
        m = bytearray(data); m[s+3] = (m[s+3] & 0xF0) | 0x02  # reserved bit set
        emit(f"{base}.frame.rsvd@{s}.flac", bytes(m))

    # 5. random byte flips
    for k in range(120):
        m = bytearray(data)
        for _ in range(random.choice((1, 1, 1, 4))):
            pos = random.randrange(4, n)
            m[pos] ^= 1 << random.randrange(8)
        emit(f"{base}.flip.{k:03d}.flac", bytes(m))

print(f"generated {count} malformed files")
