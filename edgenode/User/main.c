//这个项目位采集节点的设计，还是一样，先做硬件基础然后有了基础才可以构建应用层内容
//我们先在BSP中实现我们要实现的外设，包括LED,KEY,ESP-12S,GD25Q32,ADC,CAN,IPS,BMP280

#include "Startup/init.h"
#include <stdint.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "FreeRtos/Tasks/rtos_task.h"
#include "FreeRtos/Queue/rtos_queue.h"

/*
    现在让我们尝试完整的数据链路，不加入HMI,RS485,FAN
    我们来梳理一下
    数据链路从BMP280原始温度获取，进入message统一内部模型
    此时分三路一路用TCP通讯，一路用CAN通讯，一路给flash进行存储
    由于上发的数据都是给edgehub，我们从节点侧注意先用TCP，再用CAN
    两路都用怕引起数据重复
*/

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if(init_all() != INIT_ALL_SUCCESS)
    {
        printf("init fail");
        while(1)
        {
        }
    }

    printf("queue init start\r\n");
    if(rtos_queue_init() != RTOS_QUEUE_SUCCESS)
    {
        printf("qeueu init fail");
        while(1)
        {
        }
    }
    printf("queue init finish\r\n");
    printf("task ready init\r\n");
    storage_task_create();
    collect_task_create();
    transmit_task_create();
    hmi_task_create();
    led_tasks_create();
    stack_monitor_task_create();
    printf("Edgenode start\r\n");

    vTaskStartScheduler();

    while (1)
    {
    }
}
