#ifndef OTA_SIGNATURE_H_
#define OTA_SIGNATURE_H_

#include "ota_image.h"
#include "../Crypto/sha256/sha256.h"

typedef struct
{
    SHA256_CTX hash;
    uint8_t signature[OTA_SIGNATURE_SIZE];
    uint32_t remaining;
    uint8_t active;
} ota_signature_context_t;

/* 调用方先检查 Flash 读取及 CRC；这里绑定头字段与完整应用内容。 */
uint8_t ota_signature_begin(ota_signature_context_t *context,
                             const ota_image_header_t *header);
uint8_t ota_signature_update(ota_signature_context_t *context,
                              const uint8_t *data, uint32_t length);
uint8_t ota_signature_finish(ota_signature_context_t *context);

#endif
