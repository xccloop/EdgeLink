/*
    这个文件来写led的实现，逻辑上来说非常简单，也是我好久没写裸机的第一次学习
    | PA11 | LED1    
    | PD2  | LED2    
    对于GPIO外设的使用，我们一般分为1.启动时钟，2设置gpio口模式，3初始化位某一个电平，4对其操作
    注意某些io口不仅可以作为gpio，在某些情况下它默认为一些特殊功能的引脚入JTAG，我们必须要先看原理图然后才进行实际的操作

    PA11 44 I/O 5VT
    Default: PA11
    Alternate: USART0_CTS, CAN0_RX, USBDM, TIMER0_CH3
    PD2 54 I/O 5VT
    Default: PD2
    Alternate: TIMER2_ETI, SDIO_CMD(4), UART4_RX(4)

    从数据手册的GD32F103Rx LQFP64 pin definitions中截取看出PA11,PD2在default情况都是默认的io口，因此不用进行重映射等操作
*/
#include "led.h"
#include "gd32f10x.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"

// LED1 对应 PA11
#define LED1_PORT          GPIOA
#define LED1_PIN           GPIO_PIN_11

// LED2 对应 PD2
#define LED2_PORT          GPIOD
#define LED2_PIN           GPIO_PIN_2

void led_init()
{
    //第一步使能时钟
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOD);

    //第二部，初始化gpio配置
    gpio_init(LED1_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, LED1_PIN);
    gpio_init(LED2_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, LED2_PIN);
   /*
    这里值得重点说明的是 GPIO 的工作模式。
    GPIO_MODE_AIN            模拟输入
    GPIO_MODE_IN_FLOATING    浮空输入
    GPIO_MODE_IPD            下拉输入
    GPIO_MODE_IPU            上拉输入
    GPIO_MODE_OUT_OD         普通开漏输出
    GPIO_MODE_OUT_PP         普通推挽输出
    GPIO_MODE_AF_OD          复用开漏输出
    GPIO_MODE_AF_PP          复用推挽输出

    GPIO 从大的方向可以分为输入和输出两类。
    输入模式中，模拟输入主要用于 ADC 等模拟外设；浮空、上拉和下拉属于数字输入，其中浮空表示内部不提供默认电平，
    上拉和下拉则通过内部电阻让引脚在外部没有驱动时分别保持高电平或低电平，常用于按键等场景。通信外设的引脚模式需要根据方向判断，
    例如 UART 的 TX 一般使用复用推挽输出，RX 使用输入模式；
    SPI 主机的 SCK、MOSI 通常使用复用推挽输出，MISO 使用输入模式；IIC 的 SDA、SCL 一般使用复用开漏模式。
    输出模式主要分为推挽和开漏，推挽可以主动输出高、低电平，驱动能力较强；开漏只能主动拉低，输出 1 时实际上是关闭下拉管并进入高阻态，
    需要依靠外部上拉电阻得到高电平，因此特别适合 IIC 这种共享总线，可以避免不同设备同时输出高、低电平造成总线冲突。
    高阻态并不是高电平或低电平，而是 GPIO 不主动驱动线路，线路电平由外部电路决定。
    普通输出和复用输出的区别主要在于谁控制引脚：OUT_PP 和 OUT_OD 由 GPIO 外设控制，CPU 可以直接设置高低电平；
    AF_PP 和 AF_OD 则由 UART、SPI、IIC、TIMER 等片上外设自动控制引脚。复用和重映射也不是一个概念，复用表示 GPIO 交给片上外设使用，重映射则是改变某个外设连接到哪一组 GPIO。
    GPIO 的 2MHz、10MHz、50MHz 输出速度主要影响信号边沿速度和驱动能力，并不简单等于 GPIO 最大翻转频率，一般低速信号使用较低速度，高速 SPI、PWM 等信号再根据实际需要选择更高速度。

*/
    //第三步，设置初始电平为低（reset）
    gpio_bit_reset(LED1_PORT,LED1_PIN);
    gpio_bit_reset(LED2_PORT, LED2_PIN);
}

void led1_toggle()
{
    //这里要干的事情是不管原来的怎么样翻转一次电平
    //我们只需要读取一下现在的电平然后设置为相反的就好
    FlagStatus led1_flags = gpio_output_bit_get(LED1_PORT, LED1_PIN);
    if(led1_flags == SET)
    {
        gpio_bit_reset(LED1_PORT, LED1_PIN);
    }
    else
    {
        gpio_bit_set(LED1_PORT, LED1_PIN);
    }
}

void led1_on()
{
    gpio_bit_set(LED1_PORT, LED1_PIN);
}

void led1_off()
{
    gpio_bit_reset(LED1_PORT, LED1_PIN);
}

void led2_toggle()
{
    FlagStatus led2_flags = gpio_output_bit_get(LED2_PORT, LED2_PIN);
    if(led2_flags == SET)
    {
        gpio_bit_reset(LED2_PORT, LED2_PIN);
    }
    else
    {
        gpio_bit_set(LED2_PORT, LED2_PIN);
    }
}