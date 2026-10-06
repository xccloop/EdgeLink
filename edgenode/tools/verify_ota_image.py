#!/usr/bin/env python3
"""Verify a built A/B OTA image before direct SWD programming."""

import argparse
import struct
import sys
import zlib

sys.dont_write_bytecode = True

from make_ota_image import (
    OTA_HARDWARE_MODEL_EDGENODE_V1,
    OTA_IMAGE_FORMAT_VERSION,
    OTA_IMAGE_HEADER_SIZE,
    OTA_IMAGE_MAGIC,
    OTA_SLOT_SIZE,
    SLOTS,
    sanity_check,
)


def verify_image(image, slot):
    if len(image) < OTA_IMAGE_HEADER_SIZE + 8 or len(image) > OTA_SLOT_SIZE:
        raise ValueError("image length is outside the slot range")

    (magic, format_version, header_size, hardware_model, _firmware_version,
     target_slot, application_length, application_crc32) = struct.unpack_from(
        "<IHHIIIII", image
    )
    slot_base = SLOTS[slot]
    if (magic != OTA_IMAGE_MAGIC or
            format_version != OTA_IMAGE_FORMAT_VERSION or
            header_size != OTA_IMAGE_HEADER_SIZE or
            hardware_model != OTA_HARDWARE_MODEL_EDGENODE_V1 or
            target_slot != slot_base):
        raise ValueError("image header does not match the selected slot or hardware")

    payload = image[OTA_IMAGE_HEADER_SIZE:]
    if application_length != len(payload):
        raise ValueError("application length does not match the image file")

    expected_header_crc32 = struct.unpack_from("<I", image, 252)[0]
    if zlib.crc32(image[:252]) & 0xFFFFFFFF != expected_header_crc32:
        raise ValueError("header CRC32 mismatch")
    if zlib.crc32(payload) & 0xFFFFFFFF != application_crc32:
        raise ValueError("application CRC32 mismatch")

    msp, _reset_address = sanity_check(payload, slot_base)
    if msp & 7:
        raise ValueError("MSP is not 8-byte aligned")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--slot", choices=sorted(SLOTS), required=True)
    args = parser.parse_args()

    try:
        with open(args.input, "rb") as handle:
            verify_image(handle.read(), args.slot)
    except (OSError, ValueError) as error:
        print("OTA image verification failed: %s" % error, file=sys.stderr)
        return 1

    print("OTA image verification passed: %s" % args.input)
    return 0


if __name__ == "__main__":
    sys.exit(main())
