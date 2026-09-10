#include "board_config.h"
#include "gd32f10x.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_misc.h"
#include "gd32f10x_rcu.h"

/*
    SystemInit()在进入main()前已经完成系统时钟配置：
    SYSCLK = 72MHz，AHB = 72MHz，APB2 = 72MHz，APB1 = 36MHz。
    关于为什么是这样配置的，详情看system_gd32f10x.c
    这里不重复修改主时钟与总线分频，只配置板级公共资源。
*/

//像这种我们不希望外部去直接使用的就可以用static修饰
static void board_nvic_config(void)
{
    /* 抢占优先级和子优先级各占2位，整个工程使用同一套分组。 */
    nvic_priority_group_set(NVIC_PRIGROUP_PRE2_SUB2);

    /* USART0服务CH340，USART1服务ESP12S，二者抢占优先级最高。 */
    nvic_irq_enable(USART0_IRQn, 0U, 0U);
    nvic_irq_enable(USART1_IRQn, 0U, 1U);

    /* 三个按键共用抢占优先级2，通过子优先级确定同级响应顺序。 */
    nvic_irq_enable(EXTI1_IRQn, 2U, 0U);
    nvic_irq_enable(EXTI5_9_IRQn, 2U, 1U);
    nvic_irq_enable(EXTI10_15_IRQn, 2U, 3U);
}

static void board_debug_config(void)
{
    /* PA15连接KEY3，需关闭JTAG才能作为普通GPIO；SWD调试接口仍然保留。 */
    rcu_periph_clock_enable(RCU_AF);
    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);
}

static void board_clock_config(void)
{
    /* ADC公共时钟使用APB2/6：72MHz/6=12MHz，低于GD32F103 ADC的14MHz上限。 */
    rcu_adc_clock_config(RCU_CKADC_CKAPB2_DIV6);
}

static void board_systick_config(void)
{
    /* SystemCoreClock为72MHz，重载值72000，对应每1ms产生一次SysTick中断。 */
    SysTick_Config(SystemCoreClock / 1000U);
}

void board_config_init(void)
{
    /* 所有板级共享资源只在启动阶段配置一次。 */
    board_nvic_config();
    board_debug_config();
    board_clock_config();
    board_systick_config();
}

