#ifndef OTA_PUBLIC_KEY_H_
#define OTA_PUBLIC_KEY_H_
#include <stdint.h>
/* 固定信任根：P-256 公钥 x || y，各32字节，大端；绝不从镜像中取公钥。 */
extern const uint8_t ota_signature_public_key[64];
#endif
