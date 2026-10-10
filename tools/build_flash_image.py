#!/usr/bin/env python3
"""Builds and verifies the single image the web flasher writes at 0x0.

Reads build/flasher_args.json (never runs the build itself), lays the
parts out at their offsets, then refuses to produce anything unless the result
is an image a T-Display-P4 V1.0 will actually boot:

  * bootloader and app are ESP32-P4 images that accept chip revision 1.x
    (ESP-IDF >= 5.5.3 defaults to rev >= 3.x, which this board's bootloader
    rejects, and esptool-js does not check this for us);
  * flash header says 16 MB / DIO, as on the vendor image;
  * the partition table is byte-for-byte the vendor layout;
  * otadata is blank, so the bootloader picks ota_0 (our app) even if the unit
    had previously OTA-updated itself into ota_1;
  * every image's checksum and appended SHA-256 are correct.

Outputs docs/survivalos-p4.bin and docs/manifest.json — docs/ is the web flasher
that GitHub Pages serves.
"""
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OUT_DIR = ROOT / "docs"
IMAGE_NAME = "survivalos-p4.bin"

ESP32P4_CHIP_ID = 18
BOOTLOADER_OFFSET = 0x2000
PARTITION_TABLE_OFFSET = 0x8000
OTADATA_OFFSET = 0xF000
OTADATA_SIZE = 0x2000
APP_OFFSET = 0x20000
APP_SLOT_SIZE = 0x600000
FLASH_SIZE_CODE_16MB = 4
SPI_MODE_DIO = 2
# Highest revision the T-Display-P4 V1.0 can carry; rev >= 3.00 is a different,
# mutually exclusive build.
MAX_REV_FOR_V1_BOARD = 199
APP_DESC_MAGIC = 0xABCD5432

# name, type, subtype, offset, size — the vendor's partitions_esp32p4.csv as it
# appears in the released v1.1.2 merged image.
EXPECTED_PARTITIONS = [
    ("nvs", 1, 0x02, 0x009000, 0x006000),
    ("otadata", 1, 0x00, 0x00F000, 0x002000),
    ("phy_init", 1, 0x01, 0x011000, 0x001000),
    ("app_nvs", 1, 0x02, 0x012000, 0x00E000),
    ("ota_0", 0, 0x10, 0x020000, 0x600000),
    ("ota_1", 0, 0x11, 0x620000, 0x600000),
    ("storage", 1, 0x83, 0xC20000, 0x3E0000),
]


class Rejected(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Rejected(message)


def parse_image(data, what):
    """Validates an ESP image and returns its header facts."""
    require(len(data) >= 24, f"{what}: too short to be an image")
    magic, segments, spi_mode, size_freq = data[0], data[1], data[2], data[3]
    require(magic == 0xE9, f"{what}: bad magic 0x{magic:02X}")
    chip_id = struct.unpack_from("<H", data, 12)[0]
    min_rev, max_rev = struct.unpack_from("<HH", data, 15)
    hash_appended = data[23]
    require(chip_id == ESP32P4_CHIP_ID,
            f"{what}: chip id {chip_id}, expected {ESP32P4_CHIP_ID} (ESP32-P4)")
    require(max_rev == MAX_REV_FOR_V1_BOARD,
            f"{what}: max chip revision {max_rev / 100:.2f}; a T-Display-P4 V1.0 "
            f"needs a rev <3 build (max 1.99). Set "
            f"CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y and rebuild from clean.")
    require(min_rev <= 100,
            f"{what}: min chip revision {min_rev / 100:.2f} excludes rev 1.0 chips")

    # Walk the segments to find the checksum and the appended hash.
    offset = 24
    checksum = 0xEF
    for index in range(segments):
        require(offset + 8 <= len(data), f"{what}: segment {index} header truncated")
        _, seg_len = struct.unpack_from("<II", data, offset)
        offset += 8
        require(offset + seg_len <= len(data), f"{what}: segment {index} truncated")
        for byte in data[offset:offset + seg_len]:
            checksum ^= byte
        offset += seg_len
    # The checksum byte sits at the end of the 16-byte block the data ends in.
    offset = (offset + 16) & ~15
    require(offset <= len(data), f"{what}: checksum missing")
    require(data[offset - 1] == checksum,
            f"{what}: checksum 0x{data[offset - 1]:02X} != computed 0x{checksum:02X}")
    if hash_appended:
        require(offset + 32 <= len(data), f"{what}: appended SHA-256 missing")
        digest = hashlib.sha256(data[:offset]).digest()
        require(data[offset:offset + 32] == digest,
                f"{what}: appended SHA-256 does not match the image")
        offset += 32
    return {
        "segments": segments,
        "spi_mode": spi_mode,
        "flash_size_code": size_freq >> 4,
        "flash_freq_code": size_freq & 0xF,
        "min_rev": min_rev,
        "max_rev": max_rev,
        "image_end": offset,
        "hash_appended": bool(hash_appended),
    }


def parse_partition_table(data):
    entries = []
    md5_seen = False
    for index in range(0, len(data), 32):
        entry = data[index:index + 32]
        if entry[:2] == b"\xAA\x50":
            p_type, subtype, offset, size = struct.unpack_from("<BBII", entry, 2)
            name = entry[12:28].split(b"\0")[0].decode()
            entries.append((name, p_type, subtype, offset, size))
        elif entry[:2] == b"\xEB\xEB":
            digest = hashlib.md5(data[:index]).digest()
            require(entry[16:32] == digest, "partition table: MD5 does not match")
            md5_seen = True
            break
        else:
            break
    require(md5_seen, "partition table: MD5 entry missing")
    return entries


def main():
    args_path = BUILD / "flasher_args.json"
    require(args_path.exists(), f"{args_path} not found — build the firmware first")
    flasher_args = json.loads(args_path.read_text())

    parts = {}
    for offset_text, relative in flasher_args["flash_files"].items():
        parts[int(offset_text, 16)] = (BUILD / relative).read_bytes()

    require(set(parts) == {BOOTLOADER_OFFSET, PARTITION_TABLE_OFFSET,
                           OTADATA_OFFSET, APP_OFFSET},
            "unexpected flash layout: " + ", ".join(f"0x{o:X}" for o in sorted(parts)))
    settings = flasher_args["flash_settings"]
    require(settings["flash_size"] == "16MB" and settings["flash_mode"] == "dio",
            f"unexpected flash settings {settings}")

    bootloader = parse_image(parts[BOOTLOADER_OFFSET], "bootloader")
    require(BOOTLOADER_OFFSET + len(parts[BOOTLOADER_OFFSET]) <= PARTITION_TABLE_OFFSET,
            "bootloader overlaps the partition table")
    require(bootloader["flash_size_code"] == FLASH_SIZE_CODE_16MB,
            "bootloader header is not set to 16 MB flash")
    require(bootloader["spi_mode"] == SPI_MODE_DIO, "bootloader header is not DIO")

    partitions = parse_partition_table(parts[PARTITION_TABLE_OFFSET])
    require(partitions == EXPECTED_PARTITIONS,
            "partition table differs from the vendor layout:\n  " +
            "\n  ".join(str(p) for p in partitions))

    otadata = parts[OTADATA_OFFSET]
    require(len(otadata) == OTADATA_SIZE and otadata == b"\xFF" * OTADATA_SIZE,
            "otadata is not blank; the bootloader might not pick ota_0")

    app_bytes = parts[APP_OFFSET]
    app = parse_image(app_bytes, "app")
    require(len(app_bytes) <= APP_SLOT_SIZE,
            f"app is {len(app_bytes)} bytes, larger than the 6 MB slot")
    desc_at = 24 + 8  # image header, then the first segment header
    require(struct.unpack_from("<I", app_bytes, desc_at)[0] == APP_DESC_MAGIC,
            "app: application descriptor not found")

    def text(start, length):
        raw = app_bytes[desc_at + start:desc_at + start + length]
        return raw.split(b"\0")[0].decode()

    # The firmware embeds "SVBUILD=<stamp>"; it shows the same stamp in its
    # Control Center, so page and device can be compared at a glance.
    marker = app_bytes.find(b"SVBUILD=")
    require(marker >= 0, "app: build stamp not found (main/app/build_stamp.cpp)")
    build_stamp = app_bytes[marker + 8:marker + 40].split(b"\0")[0].decode()
    require(build_stamp and build_stamp != "unknown",
            "app: build stamp is empty; reconfigure before building")

    version, project = text(16, 32), text(48, 32)
    built = f"{text(96, 16)} {text(80, 16)}"
    idf = text(112, 32)

    image = bytearray(b"\xFF" * (APP_OFFSET + len(app_bytes)))
    for offset, data in parts.items():
        require(image[offset:offset + len(data)] == b"\xFF" * len(data),
                f"part at 0x{offset:X} overlaps another part")
        image[offset:offset + len(data)] = data
    image = bytes(image)

    # Read everything back out of the finished image, the way the device will.
    require(parse_image(image[BOOTLOADER_OFFSET:PARTITION_TABLE_OFFSET],
                        "merged bootloader")["image_end"] == bootloader["image_end"],
            "merged bootloader differs")
    require(parse_partition_table(
        image[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + 0x1000]) == partitions,
            "merged partition table differs")
    require(image[APP_OFFSET:] == app_bytes, "merged app differs")
    require(image[:BOOTLOADER_OFFSET] == b"\xFF" * BOOTLOADER_OFFSET,
            "data below the bootloader offset")

    OUT_DIR.mkdir(exist_ok=True)
    (OUT_DIR / IMAGE_NAME).write_bytes(image)
    manifest = {
        "name": "SurvivalOS",
        "board": "LILYGO T-Display-P4 V1.0",
        "version": version,
        "build": build_stamp,
        "project": project,
        "built": built,
        "idf": idf,
        "generated": time.strftime("%Y-%m-%d %H:%M"),
        "image": IMAGE_NAME,
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "md5": hashlib.md5(image).hexdigest(),
        "chip": "ESP32-P4",
        "chip_id": ESP32P4_CHIP_ID,
        "min_chip_rev": app["min_rev"],
        "max_chip_rev": app["max_rev"],
        "flash_size": "16MB",
        "offsets": {
            "bootloader": BOOTLOADER_OFFSET,
            "partition_table": PARTITION_TABLE_OFFSET,
            "otadata": OTADATA_OFFSET,
            "app": APP_OFFSET,
        },
        "app_size": len(app_bytes),
    }
    (OUT_DIR / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"bootloader  0x{BOOTLOADER_OFFSET:06X}  {len(parts[BOOTLOADER_OFFSET]):>8} B  "
          f"rev {bootloader['min_rev'] / 100:.2f}-{bootloader['max_rev'] / 100:.2f}  "
          f"checksum+sha256 OK")
    print(f"partitions  0x{PARTITION_TABLE_OFFSET:06X}  vendor layout, MD5 OK")
    print(f"otadata     0x{OTADATA_OFFSET:06X}  blank -> boots ota_0")
    print(f"app         0x{APP_OFFSET:06X}  {len(app_bytes):>8} B  "
          f"rev {app['min_rev'] / 100:.2f}-{app['max_rev'] / 100:.2f}  "
          f"checksum+sha256 OK  ({100 * len(app_bytes) // APP_SLOT_SIZE}% of slot)")
    print(f"app         {project} {version}, {idf}, build {build_stamp}")
    print(f"image       {OUT_DIR / IMAGE_NAME}  {len(image)} B")
    print(f"sha256      {manifest['sha256']}")


if __name__ == "__main__":
    try:
        main()
    except Rejected as error:
        print(f"REJECTED: {error}", file=sys.stderr)
        sys.exit(1)
