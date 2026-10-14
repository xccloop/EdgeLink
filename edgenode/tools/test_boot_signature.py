"""Run real Bootloader relay/install/slot verification with fake Flash hardware."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from cryptography.hazmat.primitives.asymmetric import ec
from make_ota_image import build_header, SLOTS

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "boot_relay.h"
#include "image_verify.h"
#include "ota_image.h"
#include "crc32.h"
#include "ExFlash/external_flash_layout.h"
static uint8_t relay[4096];
static unsigned erases, programs, saved;
static int fail_read;
static const uint8_t image[] = {SIGNED_IMAGE};
static const uint8_t image_b[] = {SIGNED_B};
const uint8_t ota_signature_public_key[64] = {PUBLIC_KEY};
uint8_t gd25_init(void) { return 1U; }
uint8_t gd25_read(uint32_t address, uint8_t *data, uint32_t length) {
    assert(address >= OTA_RELAY_BASE_ADDRESS);
    assert(address - OTA_RELAY_BASE_ADDRESS + length <= sizeof(relay));
    if(fail_read) return 0U;
    memcpy(data, relay + address - OTA_RELAY_BASE_ADDRESS, length); return 1U;
}
uint8_t gd25_invalidate(uint32_t address, uint16_t length) {
    assert(address == OTA_RELAY_BASE_ADDRESS && length == 4U);
    memset(relay, 0, length); return 1U;
}
void fmc_unlock(void) {}
void fmc_lock(void) {}
int fmc_page_erase(uint32_t address) {
    assert(address >= OTA_SLOT_A_BASE_ADDRESS && address < OTA_SLOT_B_BASE_ADDRESS);
    memset((void *)(uintptr_t)address, 0xFF, 2048U); erases++; return 0;
}
int fmc_word_program(uint32_t address, uint32_t word) {
    assert(address >= OTA_SLOT_A_BASE_ADDRESS && address + 4U <= OTA_SLOT_B_BASE_ADDRESS);
    memcpy((void *)(uintptr_t)address, &word, 4U); programs++; return 0;
}
uint8_t ota_metadata_save(const ota_metadata_t *metadata) {
    assert(metadata->pending_slot == OTA_SLOT_A_BASE_ADDRESS); saved++; return OTA_METADATA_OK;
}
void ch340_puts(const char *text) { (void)text; }
void ch340_put_hex32(uint32_t value) { (void)value; }
void ch340_put_dec(uint32_t value) { (void)value; }
static void repair(uint8_t *bytes) {
    ota_image_header_t header;
    memcpy(&header, bytes, sizeof(header));
    header.application_crc32 = crc32_generate(bytes + 256U, sizeof(image) - 256U);
    header.header_crc32 = crc32_generate((const uint8_t *)&header, 252U);
    memcpy(bytes, &header, sizeof(header));
}
int main(void) {
    void *memory;
    ota_metadata_t metadata = {0};
    unsigned mode;
#ifdef _WIN32
    memory = VirtualAlloc((void *)0x08000000, 256U*1024U, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
#else
    memory = mmap((void *)0x08000000, 256U*1024U, PROT_READ|PROT_WRITE,
                  MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
#endif
    assert(memory == (void *)0x08000000);
    memcpy((void *)(uintptr_t)OTA_SLOT_B_BASE_ADDRESS, image_b, sizeof(image_b));
    assert(image_verify(OTA_SLOT_B_BASE_ADDRESS));
    metadata.active_slot = OTA_SLOT_B_BASE_ADDRESS;
    for(mode = 0U; mode < 4U; ++mode) {
        memcpy(relay, image, sizeof(image));
        if(mode == 0U) relay[32] ^= 1U;           /* bad signature, valid CRC */
        if(mode == 1U) relay[sizeof(image)-1] ^= 1U; /* altered application */
        if(mode == 2U) relay[4] = 1U;            /* old unsigned format */
        repair(relay);
        fail_read = (mode == 3U);
        assert(!boot_relay_install(&metadata));
        assert(erases == 0U && programs == 0U && saved == 0U);
    }
    fail_read = 0;
    memcpy(relay, image, sizeof(image));
    assert(boot_relay_install(&metadata));
    assert(erases == OTA_SLOT_SIZE / 2048U && programs > 0U && saved == 1U);
    assert(image_verify(OTA_SLOT_A_BASE_ADDRESS));
    assert(image_verify(OTA_SLOT_B_BASE_ADDRESS));
    assert(!image_verify(0U));
    {
        uint8_t *slot = (uint8_t *)(uintptr_t)OTA_SLOT_A_BASE_ADDRESS;
        slot[sizeof(image)-1] ^= 1U; repair(slot);
        assert(!image_verify(OTA_SLOT_A_BASE_ADDRESS));
        memcpy(slot, image, sizeof(image)); slot[12] ^= 1U; repair(slot);
        assert(!image_verify(OTA_SLOT_A_BASE_ADDRESS));
    }
    puts("Bootloader rejects forged relay before erase and forged internal slot before boot");
#ifdef _WIN32
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, 256U*1024U);
#endif
    return 0;
}
'''


def main():
    key = ec.derive_private_key(7, ec.SECP256R1())
    app = struct.pack('<II', 0x2000C000, SLOTS['a'] + 257) + bytes(range(32))
    image = build_header(app, 10, SLOTS['a'], key) + app
    app_b = struct.pack('<II', 0x2000C000, SLOTS['b'] + 257) + bytes(range(32))
    image_b = build_header(app_b, 11, SLOTS['b'], key) + app_b
    numbers = key.public_key().public_numbers()
    public = numbers.x.to_bytes(32, 'big') + numbers.y.to_bytes(32, 'big')
    with tempfile.TemporaryDirectory(prefix='edgelink-boot-signature-') as temp:
        path = Path(temp)
        (path / 'gd32f10x_fmc.h').write_text('#include <stdint.h>\n#define FMC_READY 0\nvoid fmc_unlock(void);\nvoid fmc_lock(void);\nint fmc_page_erase(uint32_t);\nint fmc_word_program(uint32_t,uint32_t);\n')
        (path / 'gd25.h').write_text('#include <stdint.h>\nuint8_t gd25_init(void);\nuint8_t gd25_read(uint32_t,uint8_t*,uint32_t);\nuint8_t gd25_invalidate(uint32_t,uint16_t);\n')
        (path / 'CH340').mkdir()
        (path / 'CH340/ch340.h').write_text('#include <stdint.h>\nvoid ch340_puts(const char*);\nvoid ch340_put_hex32(uint32_t);\nvoid ch340_put_dec(uint32_t);\n')
        (path / 'test.c').write_text(SOURCE.replace('SIGNED_IMAGE', ','.join(str(b) for b in image)).replace('PUBLIC_KEY', ','.join(str(b) for b in public)).replace('SIGNED_B', ','.join(str(b) for b in image_b)))
        binary = path / ('test.exe' if os.name == 'nt' else 'test')
        subprocess.run([os.environ.get('HOST_CC', 'gcc'), '-std=c11', '-D_DEFAULT_SOURCE', '-O2',
                        '-Wall', '-Wextra', '-Werror', '-Wno-int-to-pointer-cast', '-Wno-unknown-pragmas',
                        '-DuECC_SUPPORTS_secp160r1=0', '-DuECC_SUPPORTS_secp192r1=0',
                        '-DuECC_SUPPORTS_secp224r1=0', '-DuECC_SUPPORTS_secp256k1=0',
                        '-DuECC_SUPPORT_COMPRESSED_POINT=0',
                        *['-I' + str(p) for p in [path, ROOT/'Common', ROOT/'Common/OTA', ROOT/'Common/CRC', ROOT/'Drivers/BSP',
                                                ROOT/'bootloader/User/Boot', ROOT/'bootloader/Driver/Core/Inc',
                                                ROOT/'Common/Crypto/micro-ecc', ROOT/'Common/Crypto/sha256']],
                        str(path/'test.c'), str(ROOT/'bootloader/User/Boot/boot_relay.c'),
                        str(ROOT/'bootloader/Driver/Core/Src/image_verify.c'),
                        str(ROOT/'Common/OTA/ota_signature.c'), str(ROOT/'Common/CRC/crc32.c'),
                        str(ROOT/'Common/Crypto/micro-ecc/uECC.c'), str(ROOT/'Common/Crypto/sha256/sha256.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
