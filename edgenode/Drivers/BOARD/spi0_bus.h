#ifndef SPI0_BUS_H_
#define SPI0_BUS_H_

#include <stdint.h>

/* SPI0 shared bus: PA5=SCK, PA6=MISO, PA7=MOSI. 仅在调度器启动前调用一次。 */
void spi0_bus_init(void);
uint8_t spi0_tansfer_data(uint8_t send_data, uint8_t *receive_data);
uint8_t spi0_bus_wait_idle(void);

/* 仅由 spi0_bus_lock() 在调度器启动后自动调用。 */
uint8_t spi0_bus_mutex_init(void);

/* 只能从任务或调度器启动前调用；ISR 不得访问 SPI0，ISR 仅通知对应任务。 */
uint8_t spi0_bus_lock(void);
void spi0_bus_unlock(void);

#endif
