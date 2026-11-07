#!/usr/bin/env python3
"""A/B 槽镜像头生成 / 校验（G6618 OTA）。

输入：槽链接 + objcopy 产出的 ``demo.bin``（前 64B 为占位头，全 0）。
输出：``slot_x.bin`` = 64B 镜像头 + payload；可选 JSON manifest 供上位机使用。

镜像头布局（小端，64B）：
    0x00 u32 signature     0x0A4D5048
    0x04 u32 magic         0x47494D47 "GIMG"
    0x08 u16 header_version = 1
    0x0A u16 slot_id       0=A 1=B
    0x0C u32 image_version
    0x10 u32 image_size    payload 字节数（不含头）
    0x14 u32 image_crc32   覆盖 payload（IEEE CRC32）
    0x18 u32 load_addr     槽基址
    0x1C u32 entry_offset  = 0x40
    0x20 u32 header_crc32  覆盖 [0x00, 0x20)
    0x24 28B reserved      全 0

用法：
    python3 scripts/gen_image_header.py --input build-slot-a/output/demo.bin \\
        --output output/slot_a.bin --slot 0 --version 42 \\
        --manifest output/ota/manifest.json
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import zlib

IMAGE_SIGNATURE = 0x0A4D5048
IMAGE_MAGIC = 0x47494D47
HEADER_VERSION = 1
HEADER_SIZE = 0x40
ENTRY_OFFSET = 0x40
HEADER_CRC_LEN = 32

SLOT_BASE = {0: 0x80020000, 1: 0x8008F000}
SLOT_SIZE = 0x0006E000
SLOT_NAMES = {0: "a", 1: "b"}

# <IIHHIIIII> = signature, magic, header_version, slot_id, image_version,
#               image_size, image_crc32, load_addr, entry_offset  （共 32B）
_HEADER_PREFIX = struct.Struct("<IIHHIIIII")


class ImageError(ValueError):
    """镜像格式错误。"""


def crc32(data: bytes) -> int:
    """IEEE CRC32（与固件 boot_crc32 一致）。"""
    return zlib.crc32(data) & 0xFFFFFFFF


def build_image(bin_data: bytes, slot: int, version: int) -> bytes:
    """把带 64B 占位头的 bin 转成带正式镜像头的槽镜像。"""
    if slot not in SLOT_BASE:
        raise ImageError(f"invalid slot {slot}")
    if len(bin_data) < HEADER_SIZE:
        raise ImageError(f"input too small: {len(bin_data)} < {HEADER_SIZE}")
    if any(bin_data[:HEADER_SIZE]):
        raise ImageError("input header placeholder is not zero (already patched?)")

    payload = bin_data[HEADER_SIZE:]
    if not payload:
        raise ImageError("empty payload")
    if len(payload) > SLOT_SIZE - HEADER_SIZE:
        raise ImageError(
            f"payload {len(payload)} exceeds slot capacity {SLOT_SIZE - HEADER_SIZE}"
        )

    prefixed = _HEADER_PREFIX.pack(
        IMAGE_SIGNATURE,
        IMAGE_MAGIC,
        HEADER_VERSION,
        slot,
        version & 0xFFFFFFFF,
        len(payload),
        crc32(payload),
        SLOT_BASE[slot],
        ENTRY_OFFSET,
    )
    header_crc = crc32(prefixed)
    header = prefixed + struct.pack("<I", header_crc) + bytes(28)

    assert len(header) == HEADER_SIZE
    return header + payload


def parse_image(data: bytes) -> dict:
    """解析并校验镜像头（不重算 payload CRC，见 verify_image）。"""
    if len(data) < HEADER_SIZE:
        raise ImageError("image too small")

    (
        signature,
        magic,
        header_version,
        slot_id,
        image_version,
        image_size,
        image_crc32,
        load_addr,
        entry_offset,
    ) = _HEADER_PREFIX.unpack_from(data, 0)
    (header_crc32,) = struct.unpack_from("<I", data, HEADER_CRC_LEN)

    if signature != IMAGE_SIGNATURE:
        raise ImageError(f"bad signature 0x{signature:08X}")
    if magic != IMAGE_MAGIC:
        raise ImageError(f"bad magic 0x{magic:08X}")
    if header_version != HEADER_VERSION:
        raise ImageError(f"unsupported header version {header_version}")
    if slot_id not in SLOT_BASE:
        raise ImageError(f"bad slot id {slot_id}")
    if load_addr != SLOT_BASE[slot_id]:
        raise ImageError(f"load_addr 0x{load_addr:08X} != slot base")
    if entry_offset != ENTRY_OFFSET:
        raise ImageError(f"bad entry offset 0x{entry_offset:X}")
    if crc32(data[:HEADER_CRC_LEN]) != header_crc32:
        raise ImageError("header crc mismatch")
    if image_size == 0 or image_size > SLOT_SIZE - HEADER_SIZE:
        raise ImageError(f"bad image size {image_size}")
    if len(data) != HEADER_SIZE + image_size:
        raise ImageError(f"file length {len(data)} != header {HEADER_SIZE + image_size}")

    return {
        "slot": slot_id,
        "slot_name": SLOT_NAMES[slot_id],
        "version": image_version,
        "image_size": image_size,
        "image_crc32": image_crc32,
        "load_addr": load_addr,
        "entry": load_addr + entry_offset,
    }


def verify_image(data: bytes, expected_slot: int | None = None) -> dict:
    """解析 + 重算 payload CRC。"""
    info = parse_image(data)
    if expected_slot is not None and info["slot"] != expected_slot:
        raise ImageError(
            f"slot mismatch: image is {info['slot_name']}, expected {SLOT_NAMES[expected_slot]}"
        )
    payload = data[HEADER_SIZE:]
    actual = crc32(payload)
    if actual != info["image_crc32"]:
        raise ImageError(
            f"payload crc mismatch: 0x{actual:08X} != 0x{info['image_crc32']:08X}"
        )
    info["verified_crc32"] = actual
    return info


def update_manifest(path: str, info: dict) -> None:
    """把槽信息合并进 manifest JSON（不存在则新建）。"""
    manifest = {"format": 1, "slots": {}}
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fp:
            manifest = json.load(fp)
            manifest.setdefault("slots", {})

    manifest["slots"][info["slot_name"]] = {
        "version": info["version"],
        "image_size": info["image_size"],
        "image_crc32": f"0x{info['image_crc32']:08X}",
        "load_addr": f"0x{info['load_addr']:08X}",
        "entry": f"0x{info['entry']:08X}",
    }
    manifest["version"] = max(
        int(s.get("version", 0)) for s in manifest["slots"].values()
    )

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(manifest, fp, indent=2, sort_keys=True)
        fp.write("\n")


def patch_elf(elf_path: str, header: bytes) -> None:
    """把 64B 镜像头写入 ELF 的 .image_header 段（调试器直接烧 ELF 也能启动）。

    ELF 是链接产物，.image_header 在文件里始终是全 0；用 .bin 烧录时头由
    build/output 的 bin 携带，但用 Ozone/J-Link 加载 .elf 时会写回全 0 头，
    导致 bootloader 拒绝启动。这里直接改写 ELF 文件中的该段。
    """
    if len(header) != HEADER_SIZE:
        raise ImageError("bad header length")

    with open(elf_path, "r+b") as fp:
        ident = fp.read(16)
        if (len(ident) < 16) or (ident[:4] != b"\x7fELF"):
            raise ImageError(f"{elf_path}: not an ELF file")
        if (ident[4] != 1) or (ident[5] != 1):
            raise ImageError(f"{elf_path}: only ELF32 little-endian is supported")

        fp.seek(0x20)
        (shoff,) = struct.unpack("<I", fp.read(4))
        fp.seek(0x2E)
        shentsize, shnum, shstrndx = struct.unpack("<HHH", fp.read(6))
        if (shentsize < 40) or (shnum == 0) or (shstrndx >= shnum):
            raise ImageError(f"{elf_path}: bad section table")

        def read_sh(index: int) -> tuple:
            fp.seek(shoff + index * shentsize)
            return struct.unpack("<IIIIIIIIII", fp.read(40))

        str_off = read_sh(shstrndx)[4]
        for i in range(shnum):
            sh = read_sh(i)
            name_off, sh_offset, sh_size = sh[0], sh[4], sh[5]
            if name_off == 0:
                continue
            fp.seek(str_off + name_off)
            name = fp.read(64).split(b"\x00", 1)[0]
            if name != b".image_header":
                continue
            if sh_size != HEADER_SIZE:
                raise ImageError(f"{elf_path}: .image_header size {sh_size} != {HEADER_SIZE}")
            fp.seek(sh_offset)
            old = fp.read(HEADER_SIZE)
            if any(old) and (old != header):
                raise ImageError(f"{elf_path}: .image_header already patched differently")
            fp.seek(sh_offset)
            fp.write(header)
            return

    raise ImageError(f"{elf_path}: .image_header section not found")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="G6618 槽镜像头生成/校验")
    parser.add_argument("--input", required=True, help="demo.bin（含 64B 占位头）或已生成的槽镜像")
    parser.add_argument("--output", default=None, help="输出槽镜像 slot_x.bin（生成模式必填）")
    parser.add_argument("--slot", required=True, type=int, choices=[0, 1])
    parser.add_argument("--version", type=int, default=0, help="镜像版本号（u32）")
    parser.add_argument("--manifest", default=None, help="可选：合并写入 manifest.json")
    parser.add_argument(
        "--elf",
        default=None,
        help="可选：把同一镜像头写入该 ELF 的 .image_header（调试器直接烧 ELF）",
    )
    parser.add_argument(
        "--verify-existing",
        action="store_true",
        help="输入已是槽镜像：只校验并写 manifest，不重新生成",
    )
    args = parser.parse_args(argv)

    try:
        with open(args.input, "rb") as fp:
            data = fp.read()
        if args.verify_existing:
            info = verify_image(data, expected_slot=args.slot)
            image = None
        else:
            if not args.output:
                parser.error("--output is required unless --verify-existing")
            image = build_image(data, args.slot, args.version)
            info = verify_image(image, expected_slot=args.slot)
    except (OSError, ImageError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    if image is not None:
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "wb") as fp:
            fp.write(image)
        if args.elf:
            patch_elf(args.elf, image[:HEADER_SIZE])

    print(
        f"slot {info['slot_name']}: version={info['version']} "
        f"payload={info['image_size']}B crc=0x{info['image_crc32']:08X} "
        f"entry=0x{info['entry']:08X}"
        + (f" -> {args.output}" if image is not None else " (verified)")
    )

    if args.manifest:
        update_manifest(args.manifest, info)
    return 0


if __name__ == "__main__":
    sys.exit(main())
