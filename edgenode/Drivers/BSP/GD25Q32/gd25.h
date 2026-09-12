#ifndef GD25_H_
#define GD25_H_

#include <stdint.h>
uint8_t gd25_init(void);
uint8_t gd25_read(uint32_t address, uint8_t *data, uint32_t length);
/* 一次最多写一页，不能跨过256字节页边界；返回失败时该页可能已经部分写入，应用层应先擦除所在扇区再重写。 */
uint8_t gd25_write(uint32_t address, const uint8_t *data, uint16_t length);
/* 擦除address所在的一个4 KiB扇区，address必须4 KiB对齐；返回失败时该扇区内容视为未知。 */
uint8_t gd25_clear(uint32_t address);

//以下为gd25常用通信命令
#define GD25Q32_CMD_WRITE_ENABLE   0x06
#define GD25Q32_CMD_READ_STATUS    0x05
#define GD25Q32_CMD_READ_DATA      0x03
#define GD25Q32_CMD_PAGE_PROGRAM   0x02
#define GD25Q32_CMD_SECTOR_ERASE   0x20
#define GD25Q32_CMD_READ_ID        0x9F

#endif
