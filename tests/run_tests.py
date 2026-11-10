#!/usr/bin/env python3
"""End-to-end tests. Usage: run_tests.py DIRECTORY_WITH_BINARIES"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from verify_image import check  # noqa: E402

BLOCK = 4096
FIXTURES = Path(__file__).parent / "fixtures"
BIN = Path(sys.argv[1]).resolve()
BUILDER = next(p for p in (BIN / "mkfs_builder", BIN / "mkfs_builder.exe") if p.exists())
ADDER = next(p for p in (BIN / "mkfs_adder", BIN / "mkfs_adder.exe") if p.exists())
passed, failed = 0, 0


def run(*cmd):
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def build(name, size=512, inodes=200):
    code, out = run(BUILDER, "--image", name, "--size-kib", size, "--inodes", inodes)
    assert code == 0, out


def add(src, dst, file):
    return run(ADDER, "--input", src, "--output", dst, "--file", file)


def add_ok(src, dst, file):
    code, out = add(src, dst, file)
    assert code == 0, out


def add_fails(src, file, message):
    Path("rejected.img").unlink(missing_ok=True)
    before = Path(src).read_bytes()
    code, out = add(src, "rejected.img", file)
    assert code != 0, f"expected failure, got: {out}"
    assert message in out, out
    assert not Path("rejected.img").exists(), "a failed add must not write an output image"
    assert Path(src).read_bytes() == before, "the input image must not change"


def clean(path, expect="."):
    errors, files = check(Path(path).read_bytes(), expect)
    assert not errors, errors
    return files


def write(name, size, byte=b"x"):
    Path(name).write_bytes(byte * size)


def test_empty_image_is_valid():
    build("a.img")
    assert clean("a.img") == {}


def test_all_size_and_inode_limits():
    for size, inodes in ((180, 128), (180, 512), (4096, 128), (4096, 512)):
        build("a.img", size, inodes)
        clean("a.img")
        assert Path("a.img").stat().st_size == size * 1024


def test_invalid_builder_arguments():
    for size, inodes in ((176, 200), (181, 200), (4100, 200), (512, 127), (512, 513)):
        code, _ = run(BUILDER, "--image", "x.img", "--size-kib", size, "--inodes", inodes)
        assert code != 0, (size, inodes)


def test_sample_files_round_trip():
    build("a.img")
    current = "a.img"
    names = sorted(p.name for p in FIXTURES.glob("file_*.txt"))
    assert len(names) == 4
    for index, name in enumerate(names):
        shutil.copy(FIXTURES / name, name)
        add_ok(current, f"b{index}.img", name)
        current = f"b{index}.img"
    assert sorted(clean(current)) == names


def test_multi_block_file_round_trip():
    build("a.img")
    Path("big.bin").write_bytes(os.urandom(30_000))
    add_ok("a.img", "b.img", "big.bin")
    assert clean("b.img")["big.bin"] == Path("big.bin").read_bytes()


def test_block_boundaries():
    build("a.img")
    current = "a.img"
    for size in (0, 1, BLOCK - 1, BLOCK, BLOCK + 1):
        write(f"s{size}", size)
        add_ok(current, f"n{size}.img", f"s{size}")
        current = f"n{size}.img"
    assert len(clean(current)) == 5


def test_largest_file_and_one_byte_more():
    build("a.img")
    write("max.bin", 12 * BLOCK)
    add_ok("a.img", "b.img", "max.bin")
    clean("b.img")
    write("over.bin", 12 * BLOCK + 1)
    add_fails("a.img", "over.bin", "direct")


def test_duplicate_name_rejected():
    build("a.img")
    write("dup.txt", 10)
    add_ok("a.img", "b.img", "dup.txt")
    add_fails("b.img", "dup.txt", "already exists")


def test_name_length_limit():
    build("a.img")
    write("n" * 57, 3)
    add_ok("a.img", "b.img", "n" * 57)
    clean("b.img")
    write("n" * 58, 3)
    add_fails("a.img", "n" * 58, "file name")


def test_corrupted_superblock_rejected():
    build("a.img")
    image = bytearray(Path("a.img").read_bytes())
    image[40] ^= 0xFF
    Path("bad.img").write_bytes(image)
    write("f.txt", 5)
    add_fails("bad.img", "f.txt", "checksum")


def test_not_an_image_rejected():
    write("junk.img", 2 * BLOCK, b"\x7f")
    write("f.txt", 5)
    add_fails("junk.img", "f.txt", "Invalid")
    write("tiny.img", 10, b"\x7f")
    add_fails("tiny.img", "f.txt", "Invalid")


def test_root_directory_fills_up():
    build("a.img", 1024, 128)
    current = "a.img"
    for index in range(62):
        name = f"f{index}"
        write(name, 1)
        add_ok(current, "next.img", name)
        shutil.move("next.img", "cur.img")
        current = "cur.img"
    assert len(clean(current)) == 62
    write("extra", 1)
    add_fails(current, "extra", "directory is full")


def test_out_of_data_blocks():
    build("a.img", 180, 128)  # 38 data blocks, one used by the root
    current = "a.img"
    for index in range(3):
        write(f"m{index}", 12 * BLOCK)
        add_ok(current, "next.img", f"m{index}")
        shutil.move("next.img", "cur.img")
        current = "cur.img"
    clean(current)
    write("last", 12 * BLOCK)
    add_fails(current, "last", "free data blocks")


def test_missing_host_file_and_directory():
    build("a.img")
    add_fails("a.img", "does_not_exist", "stat")
    os.makedirs("folder", exist_ok=True)
    add_fails("a.img", "folder", "regular file")


def main():
    global passed, failed
    tests = [(n, f) for n, f in globals().items() if n.startswith("test_")]
    for name, test in tests:
        with tempfile.TemporaryDirectory() as tmp:
            os.chdir(tmp)
            try:
                test()
                passed += 1
                print(f"PASS  {name}")
            except AssertionError as error:
                failed += 1
                print(f"FAIL  {name}: {error}")
            finally:
                os.chdir(Path(__file__).parent)
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
