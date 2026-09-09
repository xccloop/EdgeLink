#include "esp12s.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_usart.h"

/*
    这个文件我们来实现esp12s的功能实现，在硬件上我们使用PA2,PA3来进行连接ESP12S，ESP12S本质上是AT指令集，我们需要发送对应的指令让esp12s进行不同的工作
    这其实和我们的串口通信很类似，值得注意的是联网是为了实现局域网内与edgehub的TCP通信，依旧属于应用层，我们先把底层搭建好，再进行应用层的搭建

    PA2 16 I/O
    Default: PA2
    Alternate: USART1_TX, ADC012_IN2(5), TIMER1_CH2, 
    TIMER4_CH2(4), TIMER8_CH0(3)
    PA3 17 I/O
    Default: PA3
    Alternate: USART1_RX, ADC012_IN3(5), TIMER1_CH3, 
    TIMER4_CH3(4), TIMER8_CH1(3)
*/

#define ESP12S_TX_PORT GPIOA
#define ESP12S_TX_PIN GPIO_PIN_2
#define ESP12S_RX_PORT GPIOA
#define ESP12S_RX_PIN GPIO_PIN_3

#define ESP12S_BAUDRATE 115200

void esp12s_init()
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_USART1);

    gpio_init(ESP12S_TX_PORT, GPIO_MODE_AF_PP,GPIO_OSPEED_50MHZ, ESP12S_TX_PIN);
    gpio_init(ESP12S_RX_PORT, GPIO_MODE_IN_FLOATING,GPIO_OSPEED_50MHZ,ESP12S_RX_PIN);

    usart_deinit(USART1);

    usart_baudrate_set(USART1, ESP12S_BAUDRATE);

    usart_word_length_set(USART1, USART_WL_8BIT);
    usart_stop_bit_set(USART1,  USART_STB_1BIT);
    usart_parity_config(USART1, USART_PM_NONE);                 
    usart_hardware_flow_rts_config(USART1, USART_RTS_DISABLE);  
    usart_hardware_flow_cts_config(USART1, USART_CTS_DISABLE);  

    usart_receive_config(USART1, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART1, USART_TRANSMIT_ENABLE);
    usart_enable(USART1);

    nvic_irq_enable(USART1_IRQn, 0, 1);
    usart_interrupt_enable(USART1, USART_INT_RBNE);
    //到目前为止，我们进行了串口的基础配置然后接下来我们要使用ESP12s的AT指令集合用于将命令发送到ESP12s进行相关的初始化


}

//这里要做的事情就是发送字符串，我们之前使用重定向是让USART0发送，但是ESP12S通信用的是USART1，因此我们要自己实现一下
void esp12s_cmd_send(const char *data)
{
   while (*data != '\0')
    {
        while (RESET == usart_flag_get(USART1, USART_FLAG_TBE));
        usart_data_transmit(USART1, (uint8_t)*data);
        data++;
}
}