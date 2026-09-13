#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include "Model/message.h"

#define STORAGE_TEMPERATURE_RECORD_LENGTH 9
/*
    GD25一次页写不能跨256字节页。温度有效载荷为9字节，使用16字节槽位后，
    每一条记录天然落在同一页内。0~8字节是温度载荷，9~12字节是CRC32，
    第16字节是提交标记；槽位大小仍为16字节。
*/
#define STORAGE_TEMPERATURE_RECORD_SLOT_SIZE 16U

/* Storage独占GD25Q32的0x000000~0x3FFFFF地址范围，从地址0开始顺序追加。 */
#define STORAGE_BASE_ADDRESS 0x00000000UL

/*
    Storage启动入口：先验证GD25Q32，再寻找完整擦除的4 KiB扇区作为新的日志起点。
    它不会续写已有扇区，调用前需先执行board_config_init()，成功后才能追加记录。
*/
uint8_t storage_init(void);

/*
    顺序追加一条温度记录：写入载荷和CRC32后，最后提交commit并读回验证。
    进入新4 KiB扇区时先擦除一次；失败后当前启动不再继续写，避免重写半写槽位。
*/
uint8_t storage_temperature(const telemetry_sample_struct *message);

/* 只读返回RAM中的下一写入地址，供调试观察；它不等同于断电后仍存在的写指针。 */
uint32_t storage_next_address_get(void);

#define STORAGE_FAIL 0U
#define STORAGE_SUCCESS 1U

#endif
