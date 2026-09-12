#include "collection.h"
#include "BMP280/bmp280.h"

/*
    这个文件是采集的APP封装，由于我们只有BMP280，因此这里的采集也是只用bmp280
*/

float collection()
{
    float temperature = bmp280_temperature_get();
    return temperature;
}