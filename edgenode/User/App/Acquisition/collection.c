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
    float temperature_celsius;

    if((temperature == 0) || (temperature_scale == 0) || (sample_uptime_ms == 0))
    {
        return COLLECTION_FAIL;
    }

    temperature_celsius = bmp280_temperature_get();
    if(temperature_celsius == BMP280_TEMPERATURE_ERROR)
    {
        return COLLECTION_FAIL;
    }

    /* 读取完成后马上取时间；它是本次采集结果在本机产生的运行时间，不是断电后仍有效的真实日期。 */
    *sample_uptime_ms = board_systick_ms;
    *temperature_scale = -2;
    /* 转为百分之一度时明确四舍五入，避免C的强制转换总是向0截断。 */
    if(temperature_celsius >= 0.0f)
    {
        *temperature = (int32_t)(temperature_celsius * 100.0f + 0.5f);
    }
    else
    {
        *temperature = (int32_t)(temperature_celsius * 100.0f - 0.5f);
    }
    return COLLECTION_SUCCESS;
}
