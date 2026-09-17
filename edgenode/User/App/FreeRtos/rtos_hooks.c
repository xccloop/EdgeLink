/*
    #define configSUPPORT_STATIC_ALLOCATION 1
    #define configSUPPORT_DYNAMIC_ALLOCATION 0
    这两个函数是我们当初配置的，这就说明我们使用vTaskcreatestaic而不是子哦对那个分配
    因此我们需要自己手动分配任务的 TCB 和栈
*/
/*
     1. TCB（Task Control Block，任务控制块）
       FreeRTOS 中每创建一个任务，都需要保存这个任务自身的各种信息。
       比如任务当前处于 READY、BLOCKED 还是 SUSPENDED 状态、任务优先级、
       栈的位置、任务在各种链表中的节点等。
       FreeRTOS 会把这些用于管理任务的信息放在一个结构体中，这个结构体就是 TCB。
       可以简单理解为：
       TCB = FreeRTOS 用来描述和管理一个任务的“档案”。
       我们平时拿到的 TaskHandle_t，本质上就是用于找到对应任务 TCB 的句柄。
    2. 栈（Stack）
       每个任务运行时还必须拥有自己独立的栈空间。
       比如一个任务正在运行：
       void sensor_task(void *arg)
       {
           int temperature = 10;
           bmp280_read();
       }
       这里的局部变量、函数调用过程中需要保存的信息、函数返回地址等，
       都需要使用这个任务自己的栈。
       为什么每个任务必须拥有独立的栈？
       因为 FreeRTOS 会不断进行任务切换。
       例如 TaskA 运行到一半被 TaskB 抢占，TaskA 此时的运行现场不能丢失。
       FreeRTOS 会保存 TaskA 的上下文，并通过 TCB 中保存的栈相关信息找到
       TaskA 自己的栈。以后再次调度 TaskA 时，就可以恢复之前的运行现场。
       可以简单理解为：
       TCB = 记录“这个任务是谁、现在是什么状态、它的栈在哪里”
       Stack = 保存“这个任务运行过程中自己的数据和运行现场”。
        因为我们配置了：
        #define configSUPPORT_STATIC_ALLOCATION  1
        #define configSUPPORT_DYNAMIC_ALLOCATION 0
        所以 FreeRTOS 不会通过动态内存分配帮我们申请创建任务所需的 TCB 和栈，
        使用 xTaskCreateStatic() 时，需要由我们提前提供这两块内存。
        例如：
        static StaticTask_t sensor_task_tcb;
        static StackType_t sensor_task_stack[256];
        xTaskCreateStatic(
            sensor_task,
            "sensor",
            256,
            NULL,
            2,
            sensor_task_stack,
            &sensor_task_tcb
        );
        sensor_task_tcb   -> 给 FreeRTOS 保存这个任务的 TCB
        sensor_task_stack -> 给这个任务运行时作为自己的栈空间
*/
//在当前项目每创建一个静态任务，就必须为这个任务准备一份独立的 TCB 内存和一份独立的栈内存。

//现在我们按照上述描述来创建一个Freertos的idle（空闲函数），
//FreeRTOS 要创建 Idle Task，但是你又不允许它动态申请内存，那 Idle Task 的 TCB 和 Stack 从哪里来？
//于是我们只能自己创建对应的TCB,栈

#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include "rtos_hooks.h"

//这里就是我们说的TCB,栈，栈对应的大小时最小的栈空间
static StaticTask_t idle_task_tcb;
static StackType_t idle_task_stack[configMINIMAL_STACK_SIZE];

//我们前面说了idle函数是freertos内核，那自然我们只需要提供tcb和stack就好，这里用的就是专门给idle提供tcb和stack的API
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb,StackType_t **stack,configSTACK_DEPTH_TYPE *stack_size)
{
    *tcb = &idle_task_tcb;
    *stack = idle_task_stack;
    *stack_size = configMINIMAL_STACK_SIZE;
}

void rtos_assert_failed(const char *file, int line)
{
    (void)file;
    (void)line;

    taskDISABLE_INTERRUPTS();

    while (1)
    {
    }
}

/*
    这个函数并非是我们所说的tcb和stack的配置，他是栈溢出后我们进行冲断
*/
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    //这两个是显示忽略，因为这个函数强制要求传入两个形参，我们这里将它设置为void表示不关心
    //FreeRTOS 在检测到任意任务栈溢出时，统一回调的一个全局 Hook。
    //栈已经可能损坏，不能再调用printf等会继续消耗栈或等待串口的函数。
    (void)task;
    (void)task_name;

    taskDISABLE_INTERRUPTS();

    while (1)
    {
    }
}
