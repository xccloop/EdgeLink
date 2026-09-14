#include "collection.h"
#include "BMP280/bmp280.h"
#include "board_time.h"

/*
    这个文件是采集的APP封装，由于我们只有BMP280，因此这里的采集也是只用bmp280
*/

uint8_t collection_temperature_get(int32_t *temperature,
                                   int8_t *temperature_scale,
                                   uint32_t *sample_uptime_ms)
{
    bmp280_temperature_t bmp280_temperature;

    if((temperature == 0) || (temperature_scale == 0) || (sample_uptime_ms == 0))
    {
        return COLLECTION_FAIL;
    }

    if(bmp280_temperature_get(&bmp280_temperature) == BMP280_FAIL)
    {
        return COLLECTION_FAIL;
    }

    /* 读取完成后马上取时间；它是本次采集结果在本机产生的运行时间，不是断电后仍有效的真实日期。 */
    *sample_uptime_ms = board_systick_ms;
    /* BSP已经明确数值和倍率；Collection只记录采集时间并原样交给Message。 */
    *temperature = bmp280_temperature.temperature;
    *temperature_scale = bmp280_temperature.temperature_scale;
    return COLLECTION_SUCCESS;
}
