#include "board_time.h"
#include "FreeRTOS.h"
#include "task.h"

/*
    这个文件我们来配置一些关于延迟的相关函数
*/

volatile uint32_t board_systick_ms = 0;

void FreeRTOS_SysTickHandler(void);//这就是我们之间菜config里面定义的tick中断

/*
    我们发现，我们仍然是在编写系统的 SysTick 中断函数。FreeRTOS 的 SysTick 中断处理其实也在系统内部（由 port.c 提供）。
    因为 FreeRTOS 的设计，如果 port.c 通过 #define 把 xPortSysTickHandler 改名为 SysTick_Handler，就会把 SysTick_Handler 这个符号占死，
    我们就再也写不了自己的 SysTick_Handler 了。
    那为什么 PendSV_Handler 和 SVC_Handler 也和系统向量名一模一样，却能正常编译，不会冲突呢？
    核心疑问就是：
    同样是 port.c 改名占用了系统向量名，为什么 SVC / PendSV 不冲突，而 SysTick 会冲突？
    SVC / PendSV 只在调度器启动后起作用，用户从头到尾都不需要碰它们，所以让 port.c 直接占用系统向量名，靠强符号覆盖弱符号，正常工作。

    SysTick 在调度器启动前就被 BSP 用来做毫秒延时了，用户必须在它入口插一层判断，所以必须把 port.c 里的名字改成 FreeRTOS_SysTickHandler，
    腾出 SysTick_Handler 给用户自己写。
    同时也是因为我们希望改动最小而不是大幅修改，既然旧版本代码采用systick进行计数，我们就不要大幅修改
*/
void SysTick_Handler(void)
{
    //xTaskGetSchedulerState() 是 FreeRTOS 提供的 API，返回调度器当前处于哪个阶段。它的实现极其简单，本质就是读一个全局变量：
    //NOT_STARTED->没有启动，RUNNING->正在运行，SUSPENDED->被挂起了
    //如果没有启动意味着还没有进行freertos内核，我们采用旧毫秒计数
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        /*
            此时 main 还在执行初始化，FreeRTOS 尚未启动。
            继续使用旧的毫秒计时，让 ADC、IPS 等初始化延时仍可工作。
        */
        board_systick_ms++;
    }
    //如果启动了，就采用Freertos的systick，而我们在config里面启用configUSE_TICK_HOOK
    //FreeRTOS 每次处理一个 Tick 时，额外给用户提供的一个 Hook被称为vApplicationTickHook我们可以在这里面去编写一些一个tick来袭的时候我们进行的一些操作
    else
    {
        /*
            调度器启动后，SysTick 的调度工作必须交给 FreeRTOS。
            它会更新时间、唤醒延时到期任务，并按需挂起 PendSV。
        */
        FreeRTOS_SysTickHandler();
    }
}

/*
    我们在Freertos_systick函数的回调函数也board_systick_ms++
    加上前面的定时器中断函数，我们现在就做到了在任何时候board_systick_ms++
    方便我们的delay函数编写
*/
void vApplicationTickHook(void)
{
    /*
        此函数在 FreeRTOS 的 SysTick 中断中调用。
        这里只做一次加法，继续给旧 BSP/TCP 提供统一毫秒时间基准。
    */
    board_systick_ms++;
}

/*
    为什么这里还需要我们手动定义一个delay，明明freertos有专门的delay
    因为在初始化中我们首先会初始化BSP然后才初始化freertos，
    在初始化freertos之前，我们的delay就只能先用我们自己定义的。

    那如果先初始化freertos再初始化BSP呢？
    答案是：不行，或者说不是“先初始化freertos”这么简单。
    因为vTaskStartScheduler()一旦调用就不会返回，
    它会直接把CPU交给调度器，main函数后面的代码永远不会执行。
    所以BSP的初始化如果放在vTaskStartScheduler()之后，根本轮不到它跑。

    正确做法是把BSP初始化搬到一个任务里：
        xTaskCreate(bsp_init_task, ...);
        vTaskStartScheduler();   // 这里开始，main就不回来了

    然后在bsp_init_task里就可以直接用vTaskDelay：
        void bsp_init_task(void *pv)
        {
            adc_init();
            vTaskDelay(pdMS_TO_TICKS(10));
            ips_init();
            ...
            vTaskDelete(NULL);
        }

    但是这样做有两个前提：
    1. 这些BSP初始化不依赖“必须在调度器启动前完成”的假设，
       比如某些外设是FreeRTOS内核自己要用的，就不能搬。
    2. 你愿意改BSP的代码，把里面的delay_ms换成vTaskDelay。

    我们现在的做法是“改动最小”：
    - 保留BSP原有的初始化顺序，全部放在vTaskStartScheduler()之前；
    - 保留delay_ms()作为初始化阶段的延时手段；
    - SysTick在调度器启动前给delay_ms用，启动后给FreeRTOS用；
    - 通过vApplicationTickHook把board_systick_ms续上，
      这样万一有老代码在任务里误用了delay_ms，也不会卡死。

    所以不是“不能先启动FreeRTOS”，
    而是“先启动FreeRTOS就意味着BSP初始化必须搬进任务”，
    那是一次比较大的重构，我们现阶段不做。
*/
void delay_ms(uint32_t ms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        /* 调度器未启动：忙等 */
        uint32_t start = board_systick_ms;
        while ((board_systick_ms - start) < ms) {}
    }
    else
    {
        /* 调度器已启动：让出 CPU，避免阻塞其他任务 */
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

//至此我们就完成了delay的移植