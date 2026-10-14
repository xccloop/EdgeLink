"""Python signing / real C verifier interoperability and forged-image rejection."""
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib
import sys
sys.dont_write_bytecode = True

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from verify_ota_image import verify_image
from make_ota_image import build_header, SLOTS

ROOT = Path(__file__).resolve().parents[1]
# Test fixture only; this scalar is never a device signing key.
KEY = ec.derive_private_key(7, ec.SECP256R1())
APP = struct.pack('<II', 0x2000C000, SLOTS['a'] + 257) + bytes(range(256))


def image(key=KEY):
    return build_header(APP, 9, SLOTS['a'], key) + APP


def repair_crcs(data):
    data = bytearray(data)
    struct.pack_into('<I', data, 24, zlib.crc32(data[256:]) & 0xFFFFFFFF)
    struct.pack_into('<I', data, 252, zlib.crc32(data[:252]) & 0xFFFFFFFF)
    return bytes(data)


def main():
    good = image()  # Must fail on the old unsigned packer before implementation.
    public = KEY.public_key().public_numbers()
    public_bytes = public.x.to_bytes(32, 'big') + public.y.to_bytes(32, 'big')
    with tempfile.TemporaryDirectory(prefix='edgelink-signature-', ignore_cleanup_errors=True) as temp:
        path = Path(temp)
        (path / 'test.c').write_text('''
#include <string.h>
#include "OTA/ota_signature.h"
const uint8_t ota_signature_public_key[64] = {PUBLIC_KEY};
void digest(const uint8_t *image, unsigned size, uint8_t *out) {
    ota_image_header_t header; ota_signature_context_t context;
    memcpy(&header,image,256); ota_signature_begin(&context,&header);
    ota_signature_update(&context,image+256,size-256); sha256_final(&context.hash,out);
}
int verify(const uint8_t *image, unsigned size) {
    ota_image_header_t header;
    ota_signature_context_t context;
    unsigned offset;
    if(size < 264U) return 0;
    memcpy(&header, image, sizeof(header));
    if(header.application_length != size - 256U) return 0;
    if(!ota_signature_begin(&context, &header)) return -2;
    for(offset = 256U; offset < size; ++offset)
        if(!ota_signature_update(&context, image + offset, 1U)) return 0;
    return ota_signature_finish(&context);
}
'''.replace('PUBLIC_KEY', ','.join(str(b) for b in public_bytes)), encoding='utf-8')
        fixture_pem = path / 'public.pem'
        fixture_pem.write_bytes(KEY.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        verify_image(good, 'a', fixture_pem)
        fixture_private = path / 'private.pem'
        fixture_private.write_bytes(KEY.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        (path / 'app.bin').write_bytes(APP)
        # A key unrelated to the device trust anchor must fail before producing an image.
        rejected = subprocess.run([sys.executable, str(ROOT/'tools/make_ota_image.py'),
                                   '--input', str(path/'app.bin'), '--output', str(path/'bad.bin'),
                                   '--version', '9', '--slot', 'a', '--private-key', str(fixture_private)],
                                  capture_output=True)
        assert rejected.returncode != 0 and not (path/'bad.bin').exists()
        missing = subprocess.run([sys.executable, str(ROOT/'tools/make_ota_image.py'),
                                  '--input', str(path/'app.bin'), '--output', str(path/'missing.bin'),
                                  '--version', '9', '--slot', 'a', '--private-key', ''], capture_output=True)
        assert missing.returncode != 0 and not (path/'missing.bin').exists()
        # The PEM used by host tools must match the immutable C trust anchor byte for byte.
        import re
        trusted = serialization.load_pem_public_key((ROOT/'Common/OTA/ota_public_key.pem').read_bytes()).public_numbers()
        trusted_raw = trusted.x.to_bytes(32,'big') + trusted.y.to_bytes(32,'big')
        c_key = bytes(int(b,16) for b in re.findall(r'0x([0-9A-Fa-f]{2})', (ROOT/'Common/OTA/ota_public_key.c').read_text()))
        assert trusted_raw == c_key
        binary = path / ('verify.dll' if os.name == 'nt' else 'verify.so')
        subprocess.run([os.environ.get('HOST_CC', 'gcc'), '-shared', '-fPIC', '-O2',
                        '-DuECC_SUPPORTS_secp160r1=0', '-DuECC_SUPPORTS_secp192r1=0',
                        '-DuECC_SUPPORTS_secp224r1=0', '-DuECC_SUPPORTS_secp256k1=0',
                        '-DuECC_SUPPORT_COMPRESSED_POINT=0', '-DuECC_WORD_SIZE=4',
                        '-I' + str(ROOT / 'Common'), '-I' + str(ROOT / 'Common/Crypto/micro-ecc'),
                        '-I' + str(ROOT / 'Common/Crypto/sha256'),
                        str(path / 'test.c'), str(ROOT / 'Common/OTA/ota_signature.c'),
                        str(ROOT / 'Common/Crypto/micro-ecc/uECC.c'),
                        str(ROOT / 'Common/Crypto/sha256/sha256.c'), '-o', str(binary)], check=True)
        library = ctypes.CDLL(str(binary))
        library.verify.argtypes = [ctypes.c_char_p, ctypes.c_uint]
        library.verify.restype = ctypes.c_int
        def accepted(data):
            result = library.verify(data, len(data))
            return result == 1
        import hashlib
        output = ctypes.create_string_buffer(32)
        library.digest(good, len(good), output)
        normalized = bytearray(good[:252]); normalized[32:96] = bytes(64)
        assert output.raw == hashlib.sha256(bytes(normalized)+good[256:]).digest()
        assert accepted(good)
        assert not accepted(image(ec.derive_private_key(2, ec.SECP256R1())))
        for offset in [0, 4, 6, 8, 12, 16, 28, 32, 63, 95, 96, 251, 256, len(good)-1]:
            bad = bytearray(good)
            bad[offset] ^= 1
            altered = repair_crcs(bad)
            assert not accepted(altered), offset
            try:
                verify_image(altered, 'a', fixture_pem)
            except ValueError:
                pass
            else:
                raise AssertionError(('host verifier accepted forgery', offset))
        bad = bytearray(good)
        struct.pack_into('<I', bad, 20, 0xFFFFFFFF)
        assert not accepted(repair_crcs(bad))
        assert not accepted(good[:-1])
        assert not accepted(good + b'\0')
        bad = bytearray(good)
        bad[32:96] = bytes(64)
        assert not accepted(repair_crcs(bad))
        print('Signed image interoperability and forged-image rejection passed')
        if os.name == 'nt':
            import _ctypes
            _ctypes.FreeLibrary(library._handle)


if __name__ == '__main__':
    main()
