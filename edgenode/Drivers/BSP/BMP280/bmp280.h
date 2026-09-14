#ifndef BMP280_H_
#define BMP280_H_

#include <stdint.h>

#define BMP280_SUCCESS 1U
#define BMP280_FAIL    0U

/*
    BSP给上层的温度结果使用固定小数：实际温度 = temperature * 10^temperature_scale。
    例如temperature=2436、temperature_scale=-2，代表24.36℃。
    具体的量纲由BMP280驱动在这里明确，上层无需猜测整数的精度。
*/
typedef struct
{
    int32_t temperature;
    int8_t temperature_scale;
} bmp280_temperature_t;

uint8_t bmp280_init(void);
uint8_t bmp280_temperature_get(bmp280_temperature_t *temperature);

#define BMP280_CHIP_ID 0x58
#define BMP280_TEMPERATURE_SCALE (-2)

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
