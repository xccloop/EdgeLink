//这个项目位采集节点的设计，还是一样，先做硬件基础然后有了基础才可以构建应用层内容
//我们先在BSP中实现我们要实现的外设，包括LED,KEY,ESP-12S,GD25Q32,ADC,CAN,IPS,BMP280

#include "Startup/init.h"
#include "LED/led.h"
#include "Service/node_service.h"

#include <stdint.h>
#include <stdio.h>

/*
    现在让我们尝试完整的数据链路，不加入HMI,RS485,FAN
    我们来梳理一下
    数据链路从BMP280原始温度获取，进入message统一内部模型
    此时分三路一路用TCP通讯，一路用CAN通讯，一路给flash进行存储
    由于上发的数据都是给edgehub，我们从节点侧注意先用TCP，再用CAN
    两路都用怕引起数据重复
*/

int main()
{
    if(init_all() == INIT_ALL_FAIL)
    {
        while(1)
        {
        }
    }

    while(1)
    {
        /*
            开机显示
        */
        printf("Edgenode Start");

        //led显示当前系统在运行
        led1_toggle();
        led2_toggle();

        /* 一次调用只产生一条Message，并交给已经启用的输出分支。 */
        node_service_run_once();
    }
}
