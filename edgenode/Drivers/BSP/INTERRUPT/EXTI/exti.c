#include "exti.h"
#include "KEY/key.h"
#include "gd32f10x.h"
#include "gd32f10x_exti.h"

//设想中key与ips形成人机交互，所以等ips进行编写完了以后我们再回到EXTI中进行

//我们这里发现我们调用EXTI1_IRQnHandler来编写EXTI触发的时候会进行怎么样子的操作
//这是因为在startup_gd32f10x_hd.s启动文件中定义了相关函数，所以我们才可以使用他来进行操作

/*KEY3*/
void EXTI1_IRQHandler()
{
    //这里采用exti_flag_get函数来获取EXTI1此时的标志位，虽然已经进入中断了，但是以防万一我们还是再判断一次
    //这里的返回值位：typedef enum {RESET = 0, SET = !RESET} FlagStatus;
    if(exti_flag_get(EXTI_1) == SET)
    {
        key_event_record_from_isr(KEY_EVENT_3);
        exti_interrupt_flag_clear(EXTI_1);
    }
}

/*KEY2*/
void EXTI5_9_IRQHandler()
{
    if(exti_flag_get(EXTI_8) == SET)
    {
        key_event_record_from_isr(KEY_EVENT_2);
        exti_interrupt_flag_clear(EXTI_8);  
    }
}

/*KEY1*/
void EXTI10_15_IRQHandler()
{
    if(exti_flag_get(EXTI_15) == SET)
    {
        key_event_record_from_isr(KEY_EVENT_1);
        exti_interrupt_flag_clear(EXTI_15);
    }
}