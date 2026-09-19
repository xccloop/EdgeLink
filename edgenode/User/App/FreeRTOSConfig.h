#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

//这里是说明外部有一个系统时钟定义了
#if defined(__ICCARM__) || defined(__CC_ARM) || defined(__GNUC__)
    #include <stdint.h>
    #include <stdio.h>
    extern uint32_t SystemCoreClock;
#endif

//断言，configASSERT() 是 FreeRTOS 留给你的一个调试机制。
/*
    所以假设 FreeRTOS 在：
    port.c 第 415 行
    发现断言失败，程序会停止在 rtos_assert_failed()；
    此时通过 SWD 查看调用栈和 file、line 参数即可定位。
*/
void rtos_assert_failed(const char *file, int line);
#define vAssertCalled(char,int) rtos_assert_failed(char,int)
#define configASSERT(x) do { if((x)==0) { vAssertCalled(__FILE__,__LINE__); } } while(0)

#define xPortPendSVHandler  PendSV_Handler
#define vPortSVCHandler     SVC_Handler
//我们之前说Freertos会使用到异常中断即Pendsv和SVC，可是这些中断在移植之前还在MCU掌控着，我们得把它迁移到Freertos只需要用宏定义就可以
#define xPortSysTickHandler FreeRTOS_SysTickHandler
//为什么这个不直接接管系统的SysTickHandler呢？我们希望系统定时中断还是保留，作为“裸机时间 → FreeRTOS 时间”的桥。

//现在我们来定义一些freertos所需要的参数
#define configUSE_PREEMPTION 1//开启抢占调度
#define configCPU_CLOCK_HZ SystemCoreClock//之前得到在这里派上用场，用于说明系统时钟
#define configTICK_RATE_HZ 1000U//1ms一个tick，这里的tick是Freertos中的时间刻度最小单位，比如延迟（100）就是指延迟100个tick
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS//使用32位tick，1ms节拍下大约49天才会回绕一次
#define configMAX_PRIORITIES 5//配置五个优先级，01234，这里不是指仅能有5个任务，不同的任务可以获取相同的优先级
//FreeRTOS 先选“最高优先级的 Ready 队列”，然后在这个优先级内部，对多个 Ready 任务进行轮转调度。这里的部分再详细规划的时候再细说
#define configMINIMAL_STACK_SIZE 128//空闲任务的最小栈大小，单位不是字节而是32位字，实际为512字节
#define configUSE_IDLE_HOOK 0//暂时不使用空闲任务钩子函数，空闲任务只负责内核本身的清理工作
#define configUSE_TICK_HOOK 1//每次次 FreeRTOS Tick 到来时，内核会回调一次 vApplicationTickHook()。我们用它继续维护原来的 board_systick_ms。
#define configKERNEL_INTERRUPT_PRIORITY (15U << 4U)//GD32F103只有4位有效中断优先级，15是最低优先级，内核中断放在这里
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (5U << 4U)//优先级数值不小于5的中断才可以调用FreeRTOS的FromISR接口
#define configSUPPORT_STATIC_ALLOCATION 1//允许 xTaskCreateStatic(),区别于传统的xTaskCreate，传统是自动创建自动分配内存，而这个是需要我们手动分配内存
#define configSUPPORT_DYNAMIC_ALLOCATION 0//禁止 xTaskCreate()，暂不使用 FreeRTOS 暂时不使用它自己的动态内存分配机制。
#define configUSE_TIMERS	0	//暂不启用软件定时器
#define configUSE_EVENT_GROUPS	0	//暂不启用事件组
#define configUSE_STREAM_BUFFERS	0	//暂不启用流／消息缓冲区
#define configUSE_CO_ROUTINES	0	//不使用已过时的协程
#define configUSE_MUTEXES	1	//启用互斥锁
#define configUSE_TASK_NOTIFICATIONS	1	//为将来 USART/CAN 中断通知任务预留
#define INCLUDE_vTaskDelay	1	//允许任务主动延时并进入阻塞态，任务延时时其他任务可以运行
#define INCLUDE_xTaskDelayUntil	1	//允许使用周期延时 API
#define INCLUDE_xTaskGetSchedulerState	1	//允许查询调度器是否已经启动，SysTick桥接要根据它判断该走哪条路径
#define INCLUDE_uxTaskGetStackHighWaterMark 1//允许读取任务历史最小剩余栈，用于运行时检查栈是否接近溢出
#define configCHECK_FOR_STACK_OVERFLOW	2	//开启较严格的栈溢出检测
//上面的 configASSERT(x) 已经打开断言，出错后能通过 SWD 定位


#endif
