#ifndef BMP280_H_
#define BMP280_H_

#include <stdint.h>

uint8_t bmp280_init(void);
float bmp280_temperature_get(void);

#define BMP280_CHIP_ID 0x58
#define BMP280_TEMPERATURE_ERROR (-273.15f)

//以下为BMP280关键寄存器，具体可以查看手册的Page24-27
#define BMP280_REG_ID          0xD0
#define BMP280_REG_RESET       0xE0
#define BMP280_REG_STATUS      0xF3
#define BMP280_REG_CTRL_MEAS   0xF4
#define BMP280_REG_CONFIG      0xF5

#define BMP280_REG_TEMP_MSB    0xFA
#define BMP280_REG_TEMP_LSB    0xFB
#define BMP280_REG_TEMP_XLSB   0xFC

#define BMP280_REG_CALIB_START 0x88
#define BMP280_CALIB_LENGTH 24
#endif
