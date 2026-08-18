#!/usr/bin/env python3
"""Rebuild disk/disk.img from webconfig/config.htm without needing root.

disk/create.sh does this by loop-mounting a FAT image, which needs sudo. That is fine on
CI and on a machine where you can type a password, but it makes the config page awkward to
iterate on anywhere else. This does the same job by editing the committed image in place:
it rewrites only what depends on the file contents - the cluster data, the FAT chain in
both copies, and the size and start cluster of the CONFIG.HTM directory entry - and leaves
the geometry, volume label, VFAT long-name entry and timestamps exactly as create.sh
produced them.

The two are interchangeable. Rebuilding the committed image from its own config.htm with
this script reproduces it byte for byte; `--selftest` checks precisely that against
whatever git currently has, so the claim is verifiable rather than asserted. The only
difference after a real create.sh run is the directory entry's timestamp, which the FAT
driver takes from the clock.

Usage:
    misc/rebuild-disk-image.py              # disk/disk.img <- webconfig/config.htm
    misc/rebuild-disk-image.py --selftest   # prove it matches create.sh's committed output
"""
import struct
import subprocess
import sys
from pathlib import Path

# Geometry of the image create.sh makes: mkdosfs -F12 on a 2 MB volume, truncated to 64 kB.
BPS, SPC, RESERVED, FATS, SPF, ROOT_ENTRIES = 512, 4, 1, 2, 3, 512
CLUSTER = BPS * SPC                              # 2048
FAT0 = RESERVED * BPS                            # 512
ROOT = (RESERVED + FATS * SPF) * BPS             # 3584
DATA = ROOT + ROOT_ENTRIES * 32                  # 19968
FIRST_CLUSTER = 3                                # where create.sh's copy lands
IMAGE_LEN = 65536                                # create.sh keeps the first 128 sectors
NAME = b'CONFIG  HTM'


def repo_root() -> Path:
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=Path(__file__).parent,
                         capture_output=True, text=True, check=True)
    return Path(out.stdout.strip())


def fat_entry(image: bytes, n: int) -> int:
    offset = FAT0 + (n * 3) // 2
    packed = image[offset] | (image[offset + 1] << 8)
    return (packed >> 4) if (n & 1) else (packed & 0xFFF)


def set_fat_entry(buf: bytearray, n: int, value: int) -> None:
    """FAT12 packs one and a half bytes per entry, and both copies must agree."""
    for fat in (FAT0, FAT0 + SPF * BPS):
        offset = fat + (n * 3) // 2
        packed = buf[offset] | (buf[offset + 1] << 8)
        packed = ((value << 4) | (packed & 0x000F)) if (n & 1) else ((packed & 0xF000) | value)
        buf[offset], buf[offset + 1] = packed & 0xFF, (packed >> 8) & 0xFF


def dir_entry_offset(image) -> int:
    for i in range(ROOT_ENTRIES):
        offset = ROOT + i * 32
        if image[offset] == 0:
            break
        if image[offset:offset + 11] == NAME:
            return offset
    raise SystemExit(f"no {NAME.decode()} entry in the image's root directory")


def rebuild(template: bytes, payload: bytes) -> bytes:
    buf = bytearray(template)
    needed = -(-len(payload) // CLUSTER) or 1
    end = DATA + (FIRST_CLUSTER - 2 + needed) * CLUSTER

    if end > IMAGE_LEN:
        raise SystemExit(f"config.htm needs {needed} clusters ending at {end}, past the "
                         f"{IMAGE_LEN}-byte partition - the page has outgrown the image")

    start = DATA + (FIRST_CLUSTER - 2) * CLUSTER
    buf[start:end] = b'\0' * (end - start)
    buf[start:start + len(payload)] = payload

    for i in range(needed):
        set_fat_entry(buf, FIRST_CLUSTER + i,
                      0xFFF if i == needed - 1 else FIRST_CLUSTER + i + 1)

    # Free anything the previous, possibly longer, page occupied.
    cluster = FIRST_CLUSTER + needed
    while DATA + (cluster - 1) * CLUSTER <= IMAGE_LEN:
        set_fat_entry(buf, cluster, 0)
        cluster += 1

    entry = dir_entry_offset(buf)
    struct.pack_into('<H', buf, entry + 26, FIRST_CLUSTER)
    struct.pack_into('<I', buf, entry + 28, len(payload))
    return bytes(buf)


def extract(image: bytes) -> bytes:
    """Read CONFIG.HTM back out by walking the FAT, independently of how it was written."""
    entry = dir_entry_offset(image)
    size = struct.unpack('<I', image[entry + 28:entry + 32])[0]
    cluster = struct.unpack('<H', image[entry + 26:entry + 28])[0]

    chain = []
    while 2 <= cluster < 0xFF8:
        chain.append(cluster)
        cluster = fat_entry(image, cluster)

    data = b''.join(image[DATA + (n - 2) * CLUSTER:DATA + (n - 1) * CLUSTER] for n in chain)
    return data[:size], chain


def main() -> None:
    root = repo_root()
    image_path = root / "disk/disk.img"
    page_path = root / "webconfig/config.htm"

    if "--selftest" in sys.argv:
        committed = subprocess.run(["git", "show", "HEAD:disk/disk.img"], cwd=root,
                                   capture_output=True, check=True).stdout
        page = subprocess.run(["git", "show", "HEAD:webconfig/config.htm"], cwd=root,
                              capture_output=True, check=True).stdout
        rebuilt = rebuild(committed, page)
        if rebuilt == committed:
            print("selftest: rebuilding HEAD's image from HEAD's page reproduces it byte for byte")
            return
        differing = sum(a != b for a, b in zip(rebuilt, committed))
        raise SystemExit(f"selftest FAILED: {differing} bytes differ from create.sh's output")

    payload = page_path.read_bytes()
    image = rebuild(image_path.read_bytes(), payload)
    image_path.write_bytes(image)

    read_back, chain = extract(image)
    if read_back != payload:
        raise SystemExit("wrote the image but reading it back gave different bytes")

    print(f"{image_path.relative_to(root)}: {len(payload)} bytes across {len(chain)} clusters, "
          f"ending at {DATA + (chain[-1] - 1) * CLUSTER} of {IMAGE_LEN}")


if __name__ == "__main__":
    main()
