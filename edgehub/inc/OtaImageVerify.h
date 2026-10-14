#ifndef HUB_OTA_IMAGE_VERIFY_H_
#define HUB_OTA_IMAGE_VERIFY_H_

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 只读取同一个已打开的镜像文件；全部校验通过后才能开始下发。 */
int ota_image_verify_file(int file, size_t file_size);

#ifdef __cplusplus
}
#endif

#endif
