#!/usr/bin/env python3
"""Check a MiniVSFS image independently of the C code that wrote it.

Usage: verify_image.py IMAGE [--expect DIR]

With --expect, every regular file in the image must also exist in DIR with
identical contents.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

BLOCK_SIZE = 4096
INODE_SIZE = 128
DIRENT_SIZE = 64
MAGIC = 0x4D565346
SUPERBLOCK = struct.Struct("<IIIQQQQQQQQQQQQII")
INODE_HEAD = struct.Struct("<HHIIQQQQ")
DIRECT_COUNT = 12
MODE_DIR, MODE_FILE = 0o040000, 0o100000
TYPE_FILE, TYPE_DIR = 1, 2


def bit(bitmap, index):
    return bool(bitmap[index >> 3] & (1 << (index & 7)))


def check(image, expect_dir=None):
    """Return (errors, files) where files maps a name to its contents."""
    errors = []
    if len(image) < BLOCK_SIZE or len(image) % BLOCK_SIZE:
        return [f"image size {len(image)} is not a whole number of blocks"], {}

    (magic, version, block_size, total, inode_count,
     ibm_start, ibm_blocks, dbm_start, dbm_blocks,
     itable_start, itable_blocks, data_start, data_blocks,
     root_ino, _mtime, _flags, checksum) = SUPERBLOCK.unpack_from(image, 0)

    if magic != MAGIC or version != 1 or block_size != BLOCK_SIZE:
        return ["bad magic, version or block size"], {}
    if total * BLOCK_SIZE != len(image):
        errors.append("total_blocks does not match the image size")

    block = bytearray(image[:BLOCK_SIZE - 4])
    block[112:116] = b"\0" * 4
    if zlib.crc32(bytes(block)) != checksum:
        errors.append("superblock checksum mismatch")

    expected_layout = (ibm_start, dbm_start, itable_start, data_start)
    if expected_layout != (1, 2, 3, 3 + itable_blocks) or (ibm_blocks, dbm_blocks) != (1, 1):
        errors.append("unexpected region layout")
    if data_start + data_blocks != total:
        errors.append("data region does not end at the end of the image")
    if itable_blocks != (inode_count * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE:
        errors.append("inode table size does not match the inode count")

    inode_bitmap = image[ibm_start * BLOCK_SIZE:(ibm_start + 1) * BLOCK_SIZE]
    data_bitmap = image[dbm_start * BLOCK_SIZE:(dbm_start + 1) * BLOCK_SIZE]
    table = itable_start * BLOCK_SIZE

    def inode(number):
        raw = image[table + (number - 1) * INODE_SIZE:table + number * INODE_SIZE]
        mode, links, _uid, _gid, size, *_times = INODE_HEAD.unpack_from(raw, 0)
        direct = struct.unpack_from(f"<{DIRECT_COUNT}I", raw, 44)
        stored = struct.unpack_from("<Q", raw, 120)[0]
        if stored != zlib.crc32(raw[:120]):
            errors.append(f"inode {number}: checksum mismatch")
        return mode, links, size, direct

    # Every block claimed by an inode must be in range, unique and marked used.
    owners = {}

    def claim(number, direct, needed):
        for index, pointer in enumerate(direct):
            if index >= needed:
                if pointer:
                    errors.append(f"inode {number}: unused pointer {index} is not zero")
                continue
            if not data_start <= pointer < total:
                errors.append(f"inode {number}: block {pointer} is outside the data region")
                continue
            if pointer in owners:
                errors.append(f"block {pointer} is used by inodes {owners[pointer]} and {number}")
            owners[pointer] = number

    if root_ino != 1 or not bit(inode_bitmap, 0):
        return errors + ["root inode is missing"], {}
    mode, links, _size, direct = inode(1)
    if mode != MODE_DIR or links < 2:
        errors.append("root inode is not a directory")
    claim(1, direct, 1)

    root_block = image[direct[0] * BLOCK_SIZE:(direct[0] + 1) * BLOCK_SIZE]
    files, names, referenced = {}, [], set()
    for slot in range(BLOCK_SIZE // DIRENT_SIZE):
        raw = root_block[slot * DIRENT_SIZE:(slot + 1) * DIRENT_SIZE]
        number, kind = struct.unpack_from("<IB", raw, 0)
        if number == 0:
            continue
        xor = 0
        for byte in raw[:63]:
            xor ^= byte
        if xor != raw[63]:
            errors.append(f"directory entry {slot}: checksum mismatch")
        name = raw[5:63].split(b"\0", 1)[0].decode("utf-8", "replace")
        names.append(name)
        if slot < 2:
            if name != (".", "..")[slot] or number != 1 or kind != TYPE_DIR:
                errors.append(f"directory entry {slot}: expected {('.', '..')[slot]} for the root")
            continue
        if number > inode_count or not bit(inode_bitmap, number - 1):
            errors.append(f"{name}: inode {number} is not allocated")
            continue
        referenced.add(number)
        mode, links, size, direct = inode(number)
        if mode != MODE_FILE or links != 1 or kind != TYPE_FILE:
            errors.append(f"{name}: inode {number} is not a regular file")
        needed = (size + BLOCK_SIZE - 1) // BLOCK_SIZE
        if needed > DIRECT_COUNT:
            errors.append(f"{name}: size needs more than {DIRECT_COUNT} blocks")
            continue
        claim(number, direct, needed)
        content = b"".join(image[p * BLOCK_SIZE:(p + 1) * BLOCK_SIZE] for p in direct[:needed])
        files[name] = content[:size]
        if any(content[size:]):
            errors.append(f"{name}: unused space in the last block is not zero")

    if len(names) != len(set(names)):
        errors.append("duplicate names in the root directory")
    for index in range(1, inode_count):
        if bit(inode_bitmap, index) and index + 1 not in referenced:
            errors.append(f"inode {index + 1} is allocated but no directory entry uses it")
    for index in range(inode_count, BLOCK_SIZE * 8):
        if bit(inode_bitmap, index):
            errors.append(f"inode bitmap has a bit set past the inode count ({index})")
            break
    for index in range(data_blocks):
        used, claimed = bit(data_bitmap, index), data_start + index in owners
        if used != claimed:
            errors.append(f"data block {data_start + index}: bitmap says {used}, inodes say {claimed}")

    if expect_dir is not None:
        for name, content in files.items():
            path = Path(expect_dir) / name
            if not path.is_file() or path.read_bytes() != content:
                errors.append(f"{name}: contents differ from {path}")
    return errors, files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image")
    parser.add_argument("--expect")
    args = parser.parse_args()
    errors, files = check(Path(args.image).read_bytes(), args.expect)
    for error in errors:
        print(f"FAIL  {error}")
    if errors:
        return 1
    print(f"OK    {len(files)} file(s), all checksums, bitmaps and pointers consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
