#ifndef SPI0_BUS_H_
#define SPI0_BUS_H_

#include <stdint.h>

/* SPI0 shared bus: PA5=SCK, PA6=MISO, PA7=MOSI. Call once at startup. */
void spi0_bus_init(void);
uint8_t spi0_tansfer_data(uint8_t data);
void spi0_bus_wait_idle(void);

#endif
