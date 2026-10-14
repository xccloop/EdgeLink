#define _POSIX_C_SOURCE 200809L
#include "../inc/OtaImageVerify.h"
#include "../tools/ota/ota_signature.h"
#include "../tools/ota/crc32.h"
#include <errno.h>
#include <unistd.h>

static int image_read(int file, uint8_t *data, size_t length, off_t offset)
{
    while(length != 0U)
    {
        ssize_t count = pread(file, data, length, offset);
        if((count < 0) && (errno == EINTR))
        {
            continue;
        }
        if(count <= 0)
        {
            return 0;
        }
        data += count;
        length -= (size_t)count;
        offset += count;
    }
    return 1;
}

int ota_image_verify_file(int file, size_t file_size)
{
    ota_image_header_t header;
    ota_signature_context_t signature;
    uint8_t buffer[1024];
    uint32_t crc;
    uint32_t remaining;
    off_t offset = OTA_IMAGE_HEADER_SIZE;

    if((file_size < OTA_IMAGE_HEADER_SIZE + OTA_APPLICATION_MIN_LENGTH) ||
       (file_size > OTA_SLOT_SIZE) ||
       !image_read(file, (uint8_t *)&header, sizeof(header), 0) ||
       (header.application_length != file_size - OTA_IMAGE_HEADER_SIZE) ||
       !crc32_check((const uint8_t *)&header, OTA_IMAGE_HEADER_CRC_LENGTH,
                     header.header_crc32) ||
       !ota_signature_begin(&signature, &header))
    {
        return 0;
    }

    remaining = header.application_length;
    crc = crc32_begin();
    while(remaining != 0U)
    {
        uint32_t length = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        if(!image_read(file, buffer, length, offset) ||
           !ota_signature_update(&signature, buffer, length))
        {
            return 0;
        }
        crc = crc32_update(crc, buffer, length);
        offset += length;
        remaining -= length;
    }
    return (crc32_finish(crc) == header.application_crc32) &&
           ota_signature_finish(&signature);
}
