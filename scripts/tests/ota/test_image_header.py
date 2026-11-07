#!/usr/bin/env python3
"""gen_image_header.py 主机自测（纯 Python，无硬件依赖）。

覆盖：头字段布局、CRC32 与 zlib 一致、槽号/长度/占位头校验、payload CRC、
manifest 合并。
"""

from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import gen_image_header as g  # noqa: E402

checks = 0
fails = 0


def check(cond: bool, what: str) -> None:
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print(f"FAIL: {what}")


def expect_error(fn, what: str) -> None:
    global checks, fails
    checks += 1
    try:
        fn()
    except (g.ImageError, ValueError):
        return
    fails += 1
    print(f"FAIL: expected error: {what}")


def make_raw(payload: bytes) -> bytes:
    return bytes(g.HEADER_SIZE) + payload


def make_minimal_elf(path: str) -> None:
    """构造一个只含 .image_header + .shstrtab 的 ELF32 LE，用于测试 patch_elf。"""
    shstr = b"\x00" + b".image_header\x00" + b".shstrtab\x00"
    hdr_off = 0x100
    shstr_off = 0x140
    shoff = 0x180

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    ehdr = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        2,          # e_type
        243,        # e_machine (RISC-V)
        1,          # e_version
        0x80020040, # e_entry
        0,          # e_phoff
        shoff,      # e_shoff
        0,          # e_flags
        52,         # e_ehsize
        0,          # e_phentsize
        0,          # e_phnum
        40,         # e_shentsize
        3,          # e_shnum
        2,          # e_shstrndx
    )
    assert len(ehdr) == 52

    image = bytearray(b"\x00" * (shoff + 3 * 40))
    image[0:52] = ehdr
    image[hdr_off : hdr_off + g.HEADER_SIZE] = bytes(g.HEADER_SIZE)
    image[shstr_off : shstr_off + len(shstr)] = shstr

    def sh(off: int, name: int, sh_type: int, addr: int, s_off: int, size: int) -> None:
        image[off : off + 40] = struct.pack(
            "<IIIIIIIIII", name, sh_type, 0, addr, s_off, size, 0, 0, 4, 0
        )

    sh(shoff + 40, 1, 1, 0x80020000, hdr_off, g.HEADER_SIZE)  # .image_header
    sh(shoff + 80, 15, 3, 0, shstr_off, len(shstr))            # .shstrtab

    with open(path, "wb") as fp:
        fp.write(bytes(image))


def test_patch_elf() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "slot.elf")
        make_minimal_elf(path)

        header = bytes(range(1, 65))
        g.patch_elf(path, header)
        with open(path, "rb") as fp:
            data = fp.read()
        check(data[0x100 : 0x100 + 64] == header, "elf header patched")

        # 同内容重复写：幂等
        g.patch_elf(path, header)
        # 不同内容：拒绝覆盖（说明已 patched）
        expect_error(lambda: g.patch_elf(path, bytes(64)), "elf already patched")

        bad = os.path.join(tmp, "bad.elf")
        with open(bad, "wb") as fp:
            fp.write(b"not an elf")
        expect_error(lambda: g.patch_elf(bad, header), "non-elf rejected")


def main() -> int:
    payload = bytes((i * 7) & 0xFF for i in range(4096))

    check(g.crc32(b"123456789") == 0xCBF43926, "crc32 check vector")
    check(g.crc32(b"") == 0, "crc32 empty")

    image = g.build_image(make_raw(payload), slot=0, version=42)
    check(len(image) == g.HEADER_SIZE + len(payload), "image length")

    fields = struct.unpack_from("<IIHHIIIII", image, 0)
    check(fields[0] == g.IMAGE_SIGNATURE, "signature")
    check(fields[1] == g.IMAGE_MAGIC, "magic")
    check(fields[2] == 1, "header version")
    check(fields[3] == 0, "slot id")
    check(fields[4] == 42, "image version")
    check(fields[5] == len(payload), "image size")
    check(fields[6] == zlib.crc32(payload) & 0xFFFFFFFF, "image crc")
    check(fields[7] == g.SLOT_BASE[0], "load addr")
    check(fields[8] == g.ENTRY_OFFSET, "entry offset")

    (header_crc,) = struct.unpack_from("<I", image, 32)
    check(header_crc == zlib.crc32(image[:32]) & 0xFFFFFFFF, "header crc")

    info = g.verify_image(image, expected_slot=0)
    check(info["slot"] == 0 and info["version"] == 42, "verify info")
    check(info["entry"] == g.SLOT_BASE[0] + g.ENTRY_OFFSET, "entry address")

    # 槽 B
    image_b = g.build_image(make_raw(payload), slot=1, version=43)
    info_b = g.verify_image(image_b, expected_slot=1)
    check(info_b["load_addr"] == g.SLOT_BASE[1], "slot b load addr")

    # 槽号不匹配 / 长度不符 / payload 损坏
    expect_error(lambda: g.verify_image(image_b, expected_slot=0), "slot mismatch")
    expect_error(lambda: g.verify_image(image + b"\x00", expected_slot=0), "length mismatch")
    broken = bytearray(image)
    broken[-1] ^= 0xFF
    expect_error(lambda: g.verify_image(bytes(broken), expected_slot=0), "payload crc")

    # 头损坏
    broken_hdr = bytearray(image)
    broken_hdr[4] ^= 0xFF
    expect_error(lambda: g.verify_image(bytes(broken_hdr), expected_slot=0), "header crc")

    # 输入校验
    expect_error(lambda: g.build_image(bytes(32), slot=0, version=1), "short input")
    expect_error(
        lambda: g.build_image(bytes(g.HEADER_SIZE), slot=0, version=1), "empty payload"
    )
    nonzero = bytearray(make_raw(payload))
    nonzero[0] = 1
    expect_error(lambda: g.build_image(bytes(nonzero), slot=0, version=1), "placeholder not zero")
    too_big = make_raw(bytes(g.SLOT_SIZE - g.HEADER_SIZE + 1))
    expect_error(lambda: g.build_image(too_big, slot=0, version=1), "payload too large")

    # manifest 合并
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "ota", "manifest.json")
        g.update_manifest(path, g.verify_image(image, 0))
        g.update_manifest(path, g.verify_image(image_b, 1))
        with open(path, encoding="utf-8") as fp:
            manifest = json.load(fp)
        check(set(manifest["slots"].keys()) == {"a", "b"}, "manifest slots")
        check(manifest["version"] == 43, "manifest version = max")
        check(manifest["slots"]["a"]["image_crc32"].startswith("0x"), "manifest crc format")

    test_patch_elf()

    print(f"image_header host tests\n{checks} checks, {fails} failures")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
