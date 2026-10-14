"""Verify the real Hub preflight rejects forged firmware before transmission."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import utils
import zlib

SLOTS = {'a': 0x08004000, 'b': 0x08021800}


def build_header(application, version, target_slot, private_key):
    # Independent v2 wire-format fixture; no Node project imports.
    fixed = struct.pack('<IHHIIIIII', 0x3141544F, 2, 256, 1, version,
                        target_slot, len(application), zlib.crc32(application), 1)
    body = bytearray(fixed + bytes(220))
    der = private_key.sign(bytes(body) + application, ec.ECDSA(hashes.SHA256()))
    r, signature_s = utils.decode_dss_signature(der)
    body[32:96] = r.to_bytes(32,'big') + signature_s.to_bytes(32,'big')
    return bytes(body) + struct.pack('<I', zlib.crc32(body))


def main():
    if os.name == 'nt':
        raise SystemExit('Run this Linux/SocketCAN host regression in WSL or on Linux')
    key = ec.derive_private_key(7, ec.SECP256R1())
    app = struct.pack('<II', 0x2000C000, SLOTS['a'] + 257) + bytes(range(256))*5
    good = build_header(app, 10, SLOTS['a'], key) + app
    import zlib
    def repair(data):
        data = bytearray(data)
        struct.pack_into('<I', data, 24, zlib.crc32(data[256:]) & 0xFFFFFFFF)
        struct.pack_into('<I', data, 252, zlib.crc32(data[:252]) & 0xFFFFFFFF)
        return bytes(data)
    with tempfile.TemporaryDirectory(prefix='edgelink-hub-signature-') as temp:
        path = Path(temp)
        numbers = key.public_key().public_numbers()
        raw = numbers.x.to_bytes(32,'big') + numbers.y.to_bytes(32,'big')
        (path/'key.c').write_text('#include <stdint.h>\nconst uint8_t ota_signature_public_key[64] = {' + ','.join(str(b) for b in raw) + '};\n')
        (path/'test.cpp').write_text('''#include <cassert>
#include "OTA.hpp"
static unsigned sent;
Can::Can() : can_fd(-1) {}
Can::~Can() {}
bool Can::send(uint32_t, const uint8_t*, uint8_t) { ++sent; return true; }
int main(int argc, char **argv) {
    assert(argc == 2);
    Can can;
    OTA can_ota(can), tcp_ota(can);
    OTA::Result can_result, tcp_result;
    bool can_ok = can_ota.sendFirmware(1U, 3U, argv[1], can_result);
    bool tcp_ok = tcp_ota.sendFirmwareTcp(2U, 3U, argv[1], tcp_result);
    assert(can_ok == tcp_ok && sent == 0U);
    assert(can_ota.isSending() == can_ok && tcp_ota.isSendingTcp() == tcp_ok);
    if(!can_ok) {
        assert(can_result.error == OTA::Error::InvalidImage || can_result.error == OTA::Error::InvalidFile);
        assert(tcp_result.error == can_result.error);
    }
    return can_ok ? 0 : 1;
}
''')
        binary = path/('test.exe' if os.name=='nt' else 'test')
        sources = [path/'key.c', ROOT/'src/OtaImageVerify.c',
                   ROOT/'tools/ota/ota_signature.c',
                   ROOT/'tools/ota/crc32.c',
                   ROOT/'tools/crypto/sha256/sha256.c',
                   ROOT/'tools/crypto/micro-ecc/uECC.c']
        objects = []
        for index, source in enumerate(sources):
            obj = path/('%d.o' % index)
            subprocess.run([os.environ.get('HOST_CC','gcc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2',
                            '-DuECC_SUPPORTS_secp160r1=0', '-DuECC_SUPPORTS_secp192r1=0',
                            '-DuECC_SUPPORTS_secp224r1=0', '-DuECC_SUPPORTS_secp256k1=0',
                            '-DuECC_SUPPORT_COMPRESSED_POINT=0',
                            '-I'+str(ROOT/'src'), '-I'+str(ROOT/'tools/ota'), '-I'+str(ROOT/'inc'),
                            '-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        subprocess.run([os.environ.get('HOST_CXX','g++'), '-std=c++17', '-O2',
                        '-I'+str(ROOT/'inc'), '-I'+str(ROOT/'src'),
                        str(path/'test.cpp'), str(ROOT/'src/OTA.cpp'), *objects,
                        '-o', str(binary)], check=True)
        def accepted(data):
            (path/'image.bin').write_bytes(data)
            result = subprocess.run([str(binary),str(path/'image.bin')])
            assert result.returncode in (0,1)
            return result.returncode == 0
        assert accepted(good)
        other_key = ec.derive_private_key(8,ec.SECP256R1())
        assert not accepted(build_header(app,10,SLOTS['a'],other_key)+app)
        for offset in [0,4,8,12,16,28,32,95,96,251,256,1000,len(good)-1]:
            bad=bytearray(good);bad[offset]^=1
            assert not accepted(repair(bad)), offset
        old = bytearray(good)
        struct.pack_into('<H', old, 4, 1)
        assert not accepted(repair(old))
        assert not accepted(good[:-1])
        assert not accepted(good+b'\0')
        assert not accepted(b'')
        print('Hub signature preflight accepts valid images and rejects forged files')


if __name__=='__main__':
    main()
