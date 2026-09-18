#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include "Model/message.h"

/* Flash 中每条遥测记录固定占 16 字节。 */
#define STORAGE_SLOT_SIZE 16U

/* Storage 函数统一返回值。 */
#define STORAGE_SUCCESS 1U
#define STORAGE_FAIL    0U

uint8_t storage_init(uint32_t *recovered_next_sequence);

uint8_t storage_write_pending(const telemetry_sample_struct *message,uint32_t *flash_address);


#endif
