#include "ch340.h"
#include "gd32f10x.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_usart.h"
/*
    这个文件我们来实现串口的实现,我们使用PA9,PA10来进行mcu与主机的通信，作为调试的串口，我们需要他具有发送任意字节和可以接受任意字节的能力
    因此在我们要实现串口的接受中断使能+printf重定向

    PA9 42 I/O 5VT
    Default: PA9
    Alternate: USART0_TX, TIMER0_CH1
    PA10 43 I/O 5VT
    Default: PA10
    Alternate: USART0_RX, TIMER0_CH2
*/

#define CH340_TX_PORT GPIOA
#define CH340_TX_PIN GPIO_PIN_9

#define CH340_RX_PORT GPIOA
#define CH340_RX_PIN GPIO_PIN_10

#define CH340_BAUDRATE 115200

void ch340_init()
{
    //这一步本来想配一个宏增加可读性，想了一下算了吧，好麻烦
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_AF);

    gpio_pin_remap_config(GPIO_USART0_REMAP, ENABLE);
    //前两步就是简单的时钟使能+重映射

    //记得吗，我们之前讨论gpio的八种模式，对于tx这种不需要接受数据反而要求要数据可靠性的我们使用推挽输出，同时我们使用复用，这里采用复用推挽
    //而对于RX接受端来说，我们希望它数据可以接近原始所以采用浮空输入
    gpio_init(CH340_TX_PORT, GPIO_MODE_AF_PP,GPIO_OSPEED_50MHZ, CH340_TX_PORT);
    gpio_init(CH340_RX_PORT, GPIO_MODE_IN_FLOATING,GPIO_OSPEED_50MHZ,CH340_RX_PIN);

    //接下来就是串口的配置
    //这一步为串口的默认化，我们是USART0
    usart_deinit(USART0);

    //设置波特率
    usart_baudrate_set(USART0, CH340_BAUDRATE);

    /*
        接下来我们定义数据发送的格式
        usart_word_length_set是设置发送的数据中一共有几位，可选的参数有USART_WL_8BIT，USART_WL_9BIT，我们这里选择最经典的八位
        usart_stop_bit_set用于设置停止位，我们这里选用USART_STB_1BIT代表着使用1为停止位，以此类推USART_STB_2BIT就是使用两位
    */
    usart_word_length_set(USART0, USART_WL_8BIT);
    usart_stop_bit_set(USART0,  USART_STB_1BIT);
    usart_parity_config(USART0, USART_PM_NONE);                 // 奇偶校验位
    usart_hardware_flow_rts_config(USART0, USART_RTS_DISABLE);  // 硬件流控制RTS
    usart_hardware_flow_cts_config(USART0, USART_CTS_DISABLE);  // 硬件流控制CTS

    //准备工作做完，接下来我们开启usart的收发和usart的使能,关于使能的宏可以自行查看库函数，这里不过多赘述
    usart_receive_config(USART0, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_enable(USART0);

    //最后我们设置串口中断，和设置EXTI中断差别不大，这里不过多赘述
    // 使能USART中断
    nvic_irq_enable(USART0_IRQn, 0, 0);
    // 使能串口接收中断
    usart_interrupt_enable(USART0, USART_INT_RBNE);

}

