#include "board_config.h"
#include "spi0_bus.h"
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
    /*
        在 Cortex-M FreeRTOS 工程里，把全部实现的 NVIC 优先级位用于抢占优先级，是最简单、最不容易出错的配置方式，尤其当 ISR 要调用 FreeRTOS API 时
        FreeRTOS 的中断 API 安全规则只适合使用完整抢占优先级，我们修改为
        4 位全部用于“谁能打断谁”，不再使用子优先级，同理下面的优先级我们也要修改，去除子优先级
    */
    nvic_priority_group_set(NVIC_PRIGROUP_PRE4_SUB0);

    /*
        USART0 服务 CH340，USART1 服务 ESP12S。
        两者使用相同抢占优先级，因此它们之间不能互相抢占。
    */
    nvic_irq_enable(USART0_IRQn, 5U, 0U);
    nvic_irq_enable(USART1_IRQn, 5U, 0U);

    /*
        三个按键中断优先级较低。
        它们使用相同抢占优先级，因此彼此之间不能互相抢占。
    */
    nvic_irq_enable(EXTI1_IRQn, 10U, 0U);
    nvic_irq_enable(EXTI5_9_IRQn, 10U, 0U);
    nvic_irq_enable(EXTI10_15_IRQn, 10U, 0U);

    /* CAN接受中断 */
    nvic_irq_enable(USBD_LP_CAN0_RX0_IRQn, 15U, 0U);
}

static void board_debug_config(void)
{
    /* PA15连接KEY3、PB3连接IPS的SPI2时钟，二者都需关闭JTAG；SWD调试接口仍然保留。 */
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

static void board_spi0_device_cs_safe_state(void)
{
    /*
        PA5/PA6/PA7是SPI0共享总线，而PA4和PB12分别是两颗从设备独立的片选。
        上电后若某个CS保持浮空或低电平，未初始化的从设备也可能同时驱动MISO；
        因此先把所有已知SPI0从设备设为未选中，再开始SPI0总线初始化。
    */
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);

    gpio_bit_set(GPIOA, GPIO_PIN_4);
    gpio_init(GPIOA, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_4);

    gpio_bit_set(GPIOB, GPIO_PIN_12);
    gpio_init(GPIOB, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_12);
}

void board_config_init(void)
{
    /* 所有板级共享资源只在启动阶段配置一次。 */
    board_nvic_config();
    board_debug_config();
    board_clock_config();
    board_systick_config();
    board_spi0_device_cs_safe_state();
    spi0_bus_init();
}
