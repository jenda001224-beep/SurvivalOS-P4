#!/usr/bin/env python3
"""Packs the ESP32-C6 network-adapter firmware for embedding in SurvivalOS.

The T-Display-P4 V1.0 shipped with ESP-AT on its ESP32-C6, which ESP-Hosted
cannot talk to. SurvivalOS carries the vendor's ESP-Hosted image and writes it
to the ESP32-C6 over SDIO itself (main/app/coprocessor). This tool turns the
vendor's merged image (flashed at 0x0) into the container the firmware embeds:

    offset  size  field
    0       4     magic "SVC6"
    4       4     image size in bytes, little endian (uncompressed)
    8       32    MD5 of the uncompressed image, lowercase hex
    40      ...   zlib stream of the image

The flasher stub on the ESP32-C6 inflates the stream, so the phone never has
to hold the uncompressed image.

    tools/pack_coprocessor_firmware.py <merged.bin> <output.svc6>
"""
import hashlib
import struct
import sys
import zlib

ESP_IMAGE_MAGIC = 0xE9
ESP32C6_CHIP_ID = 13
PARTITION_TABLE_OFFSET = 0x8000
FLASH_SIZE_4MB = 2


def fail(message):
    sys.exit(f"pack_coprocessor_firmware: {message}")


def main():
    if len(sys.argv) != 3:
        fail("usage: pack_coprocessor_firmware.py <merged.bin> <output.svc6>")
    image = open(sys.argv[1], "rb").read()

    # Refuse anything that is not a merged ESP32-C6 image starting at 0x0: a
    # wrong file here would be written over the co-processor's bootloader.
    if len(image) < PARTITION_TABLE_OFFSET + 32 or image[0] != ESP_IMAGE_MAGIC:
        fail("not an ESP image that starts with a bootloader")
    chip_id = struct.unpack_from("<H", image, 12)[0]
    if chip_id != ESP32C6_CHIP_ID:
        fail(f"image is for chip id {chip_id}, expected ESP32-C6 ({ESP32C6_CHIP_ID})")
    if image[3] >> 4 != FLASH_SIZE_4MB:
        fail("image is not built for the 4 MB flash of the ESP32-C6-MINI-1-N4")
    if image[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + 2] != b"\xaa\x50":
        fail("no partition table at 0x8000: is this the merged image?")
    if len(image) % 4 != 0:
        image += b"\xff" * (4 - len(image) % 4)

    packed = zlib.compress(image, 9)
    header = b"SVC6" + struct.pack("<I", len(image))
    header += hashlib.md5(image).hexdigest().encode("ascii")
    with open(sys.argv[2], "wb") as output:
        output.write(header + packed)
    print(f"image {len(image)} B, md5 {hashlib.md5(image).hexdigest()}, "
          f"packed {len(header) + len(packed)} B -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
