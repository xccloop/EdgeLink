#!/usr/bin/env python3
"""Verify a built A/B OTA image before direct SWD programming."""

import argparse
import struct
import sys
import zlib
from pathlib import Path
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

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


DEFAULT_PUBLIC_KEY = Path(__file__).resolve().parents[1] / "Common/OTA/ota_public_key.pem"


def verify_image(image, slot, public_key_path=DEFAULT_PUBLIC_KEY):
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

    if struct.unpack_from("<I", image, 28)[0] != 1:
        raise ValueError("unsupported signature algorithm")
    with open(public_key_path, "rb") as handle:
        public_key = serialization.load_pem_public_key(handle.read())
    if not isinstance(public_key, ec.EllipticCurvePublicKey) or not isinstance(public_key.curve, ec.SECP256R1):
        raise ValueError("verification key must be ECDSA P-256")
    normalized = bytearray(image[:252])
    normalized[32:96] = bytes(64)
    r = int.from_bytes(image[32:64], "big")
    signature_s = int.from_bytes(image[64:96], "big")
    try:
        public_key.verify(utils.encode_dss_signature(r, signature_s),
                          bytes(normalized) + payload, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature as error:
        raise ValueError("firmware signature mismatch") from error

    msp, _reset_address = sanity_check(payload, slot_base)
    if msp & 7:
        raise ValueError("MSP is not 8-byte aligned")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--slot", choices=sorted(SLOTS), required=True)
    parser.add_argument("--public-key", type=Path, default=DEFAULT_PUBLIC_KEY)
    args = parser.parse_args()

    try:
        with open(args.input, "rb") as handle:
            verify_image(handle.read(), args.slot, args.public_key)
    except (OSError, ValueError) as error:
        print("OTA image verification failed: %s" % error, file=sys.stderr)
        return 1

    print("OTA image verification passed: %s" % args.input)
    return 0


if __name__ == "__main__":
    sys.exit(main())
