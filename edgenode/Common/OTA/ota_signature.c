#include "ota_signature.h"
#include "ota_public_key.h"
#include "../Crypto/micro-ecc/uECC.h"
#include <string.h>

uint8_t ota_signature_begin(ota_signature_context_t *context,
                             const ota_image_header_t *header)
{
    static const uint8_t empty_signature[OTA_SIGNATURE_SIZE] = {0};
    const uint8_t *bytes = (const uint8_t *)header;

    if(context == NULL)
    {
        return 0U;
    }
    context->active = 0U;
    if((header == NULL) || (header->magic != OTA_IMAGE_MAGIC) ||
       (header->header_version != OTA_IMAGE_FORMAT_VERSION) ||
       (header->header_size != OTA_IMAGE_HEADER_SIZE) ||
       (header->hardware_model != OTA_CURRENT_HARDWARE_MODEL) ||
       (header->signature_algorithm != OTA_SIGNATURE_ECDSA_P256_SHA256) ||
       ((header->target_slot != OTA_SLOT_A_BASE_ADDRESS) &&
        (header->target_slot != OTA_SLOT_B_BASE_ADDRESS)) ||
       (header->application_length < OTA_APPLICATION_MIN_LENGTH) ||
       (header->application_length > OTA_SLOT_SIZE - OTA_APPLICATION_OFFSET))
    {
        return 0U;
    }

    sha256_init(&context->hash);
    sha256_update(&context->hash, bytes, OTA_SIGNATURE_OFFSET);
    /* 签名本身按全零参与摘要；尾部 CRC 不参与，避免循环依赖。 */
    sha256_update(&context->hash, empty_signature, sizeof(empty_signature));
    sha256_update(&context->hash, bytes + OTA_SIGNATURE_OFFSET + OTA_SIGNATURE_SIZE,
                  OTA_IMAGE_HEADER_DATA_SIZE - OTA_SIGNATURE_OFFSET - OTA_SIGNATURE_SIZE);
    memcpy(context->signature, header->signature, OTA_SIGNATURE_SIZE);
    context->remaining = header->application_length;
    context->active = 1U;
    return 1U;
}

uint8_t ota_signature_update(ota_signature_context_t *context,
                              const uint8_t *data, uint32_t length)
{
    if((context == NULL) || (context->active == 0U))
    {
        return 0U;
    }
    if((length > context->remaining) || ((length != 0U) && (data == NULL)))
    {
        context->active = 0U;
        return 0U;
    }
    if(length != 0U)
    {
        sha256_update(&context->hash, data, length);
    }
    context->remaining -= length;
    return 1U;
}

uint8_t ota_signature_finish(ota_signature_context_t *context)
{
    uint8_t digest[SHA256_BLOCK_SIZE];

    if((context == NULL) || (context->active == 0U))
    {
        return 0U;
    }
    context->active = 0U;
    if(context->remaining != 0U)
    {
        return 0U;
    }
    sha256_final(&context->hash, digest);
    return (uECC_verify(ota_signature_public_key, digest, sizeof(digest),
                         context->signature, uECC_secp256r1()) == 1) ? 1U : 0U;
}
