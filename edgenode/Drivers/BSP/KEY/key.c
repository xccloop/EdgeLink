#include "key.h"
#include <stdio.h>
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_exti.h"

/*
    这个文件我们来进行key的相关操作，主要是检测是否按下，这三个按键都是低电平有效
    | PA15 | KEY1
    | PA8  | KEY2 
    | PC1  | KEY3
    
    PC1 9 I/O
    Default: PC1
    Alternate: ADC012_IN11(5)
    PA8 41 I/O 5VT
    Default: PA8
    Alternate: USART0_CK, TIMER0_CH0, CK_OUT0
    PA15 50 I/O 5VT
    Default: JTDI
    Alternate: SPI2_NSS(4), I2S2_WS(4)
    Remap: TIMER1_CH0, TIMER1_ETI, PA15, SPI0_NSS

    与led的gpio控制不同，我们看到PA15的default为JTDI并非普通的PA15，board_config_init会将其重映射为普通IO。
    与led的简单高低输出不同，对于按键，为了后续加入freertos，我们先进行中断+标志位的方式实现
*/

// KEY 编号 <-> 引脚 <-> EXTI 线 的对应（本次把 KEY1 与 KEY3 对调）：
//   KEY1 = PA15 -> EXTI15
//   KEY2 = PA8  -> EXTI8
//   KEY3 = PC1  -> EXTI1
#define KEY1_PORT GPIOA
#define KEY1_PIN GPIO_PIN_15
#define KEY2_PORT GPIOA
#define KEY2_PIN GPIO_PIN_8
#define KEY3_PORT GPIOC
#define KEY3_PIN GPIO_PIN_1

void key_init()
{
    //这次的初始化要考虑到的有，时钟和中断
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOC);

    /*
        关于JTAG有三个宏给我们使用
        GPIO_SWJ_NONJTRST_REMAP          !< full SWJ(JTAG-DP + SW-DP),but without NJTRST 
        GPIO_SWJ_SWDPENABLE_REMAP        !< JTAG-DP disabled and SW-DP enabled 
        GPIO_SWJ_DISABLE_REMAP           !< JTAG-DP disabled and SW-DP disabled 
        分别代表着完整JTAG+SWD、禁用JTAG保留SWD、禁用JTAG和SWD。
        PA15的复用选择及AFIO时钟由board_config_init统一完成。
    */

    //由于我们的按键时低电平有效，因此设置为上拉输入
    gpio_init(KEY1_PORT, GPIO_MODE_IPU , GPIO_OSPEED_50MHZ, KEY1_PIN);
    gpio_init(KEY2_PORT, GPIO_MODE_IPU , GPIO_OSPEED_50MHZ, KEY2_PIN);
    gpio_init(KEY3_PORT, GPIO_MODE_IPU , GPIO_OSPEED_50MHZ, KEY3_PIN);

    gpio_bit_set(KEY1_PORT,KEY1_PIN);
    gpio_bit_set(KEY2_PORT,KEY2_PIN);
    gpio_bit_set(KEY3_PORT,KEY3_PIN);

    /*
        接下来是关于中断的部分
        中断分为很多种，基本上我们常见的外设上面都设有中断，如串口中断，SPI中断等等等，我们这里的中断是用与检测GPIO引脚变化的中断，所以我们需要使用外部中断（exti）
        配置 EXTI 信号源的时候需要用到 AFIO 的外部中断控制寄存器 AFIO_EXTISSx，AFIO时钟已由board_config_init开启。
        通过查询数据手册后发现，EXTI中断源对应的EXTI事件是一一对应的编号，如我们的PC1,PA8,PA15,分别代表着exti1，exti8，exti15
    */
    //现在我们来配置exti来源选择
    gpio_exti_source_select(GPIO_PORT_SOURCE_GPIOC, GPIO_PIN_SOURCE_1);
    gpio_exti_source_select(GPIO_PORT_SOURCE_GPIOA, GPIO_PIN_SOURCE_8);
    gpio_exti_source_select(GPIO_PORT_SOURCE_GPIOA, GPIO_PIN_SOURCE_15);

    /*
        现在我们来初始化exti
        exti_init有三个形参，第一个参数为exti事件触发源，我们之前讨论的exti事件也就是PC1,PA8,PA15,分别代表着exti1，exti8，exti15
        第二个参数，为配置EXTI中断模式，这个模式下我们需要编写中断函数，也是我们使用exti中断使用的模型，还有另外一个形参EXTI_EVENT，这个模式为exti触发但仅仅产生事件，不产生中断
        第三个形参为设置exti为下降沿触发，我们之前说exti为外部中断那具体外部在电压什么情况下我们才触发中断呢？
        我们之前说我们的按键时低电平有效也就是从1到0的那一刻是我们需要的，因此我们设置为下降沿触发，一共有这四种供我们使用
        EXTI_TRIG_RISING = 0,     上升沿触发
        EXTI_TRIG_FALLING,        下降沿触发                            
        EXTI_TRIG_BOTH,           上升和下降都触发                                     
        EXTI_TRIG_NONE            不触发任何中断                                      
    */
    exti_init(EXTI_1, EXTI_INTERRUPT, EXTI_TRIG_FALLING);
    exti_init(EXTI_8, EXTI_INTERRUPT, EXTI_TRIG_FALLING);
    exti_init(EXTI_15, EXTI_INTERRUPT, EXTI_TRIG_FALLING);

    /*
        我们之前所说在使用Freertos之前我们采用中断+标志位的方式，这里的标志位并不需要我们去定义，而是在内部当中断触发的时候，标志位就会置1，应该是中断寄存器帮我的
        因此在使用之前我们要像GPIO设置确定电平一样，设置标志位为0也就是清空
    */
    exti_interrupt_flag_clear(EXTI_1);
    exti_interrupt_flag_clear(EXTI_8);
    exti_interrupt_flag_clear(EXTI_15); 

    //KEY的EXTI NVIC优先级由board_config_init统一配置。
    printf("\nKEY init finsh\n");
}
