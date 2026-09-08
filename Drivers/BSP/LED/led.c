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
        这里值得说的是gpio的工作模式
        GPIO_MODE_AIN	        模拟输入	
        GPIO_MODE_IN_FLOATING	浮空输入	  
        GPIO_MODE_IPD	        下拉输入 
        GPIO_MODE_IPU	        上拉输入	
        GPIO_MODE_OUT_OD	    开漏输出	
        GPIO_MODE_OUT_PP	    推挽输出	
        GPIO_MODE_OUT_OD	    复用开漏输出	
        GPIO_MODE_OUT_PP	    复用推挽输出	

        分为两大类：输入，输出
        关于输入，代表着是用与读取外部信号的，模拟输入：它用于读取模拟信号，我们常用的ADC采用的就是模拟输入
        关于浮空，上拉，下拉输入，它代表着这个io口内部是否有上下拉电阻，上下拉都是为了在外部情况不稳定的时候，内部读取到的信号是一个稳定的值，我也可以用于检测高低电平
        而浮空代表着完全由外部信号决定，我们使用的串口RX,SPI,IIC都使用浮空输入
        关于输出，我们观察到还可以细分为开漏和推挽，开漏：意味着这个口输出能力强，他会明确的输出是高电平还是低电平
        而开漏输出，他可以输出低电平，但是由于内部仅有一个下拉nmos，所以无法输出高电平，如果我们尝试输出高电平实际上只会让io口变为高阻态，如果想要输出高电平就需要在单片机的外部加一个上拉电阻

        我们发现关于输出，推挽的能力比开漏要强得多，为什么不能直接替代呢？这里引出另外一个问题，我们什么时候会需要用到开漏输出
        现在有这么一个情况：我们需要用IIC连接其余外设，由于IIC为总线制，也就是说一个线上会有多个外设连接
        而IIC的通信方式是在一条总线上，我们即需要可以将数据发送出去有需要外设通过总线吧数据传输回来，如果我们此时使用推挽输出，那么虽然可以将数据发送出去，但是数据收回来却会产生问题
        比如此时外设输出1，而单片机输出0，就会发现此时电流会从外部到内部的下拉mos管短路！而如果用开漏输出则不会出现这种情况
        开漏输出只有低电平和高阻态，也就意味着数据要输出的时候我们只需要设为1（高阻态），那数据就可以安全的传到单片机内部供我们读取
        还有一这情况，我们知道设置为1变成高阻态，那如果此时我们在外部加一个5V的上拉电阻，当变成高阻态的时候，外部电阻把电平拉到5V；当想输出0时，它拉低引脚到0V。
        于是我们就用一个3.3V的单片机外设口去使用了一个5V的电压。
        所以我们发现在需要使用共享或者电压转化的情况下，开漏输出的高阻态是非常有用的

        这里介绍一下高阻态，所谓的高阻态就是对外既不是低电平也不是高电平而是会显示有一个很大的电阻，那么这个时候电压的高低完全由外部决定
        虽然高阻态测得的电平也是0V，但这并不意味着就是低电平，因为这相当于外部给的是低电平，而如果外部是高电平那也会变成高电平
        高低电平只是结果并非高阻态的能力

        现在我们来看最后两个也就是复用的推挽和开漏
        复用代表着我们使用了硬件外设作为个gpio口，因此我们需要先复用再配置，比如SPI,IIC等，我们之前讨论的开漏可以注意到在IIC情况下，还是需要我们手动的去设置高低电平
        分时间来获取或者输出，但是复用的话，意味着我们用到的硬件的IIC，就可以帮助我们省去CPU的占用，不过要注意的是，复用是复用，我们依旧得分情况来使用复用开漏还是复用推挽
    */
    /*
        我们还注意到设置GPIO的第三个值是speed，他有三个可以设置的，这个值代表着io口输出速率的最大限制
        GPIO_OSPEED_10MHZ	输出速度最大为10MHz	output max speed 10MHz
        GPIO_OSPEED_2MHZ	输出速度最大为2MHz	output max speed 2MHz
        GPIO_OSPEED_50MHZ	输出速度最大为50MHz	output max speed 50MHz
        50mhz是最大，它意味着高低电平切换的信号沿会很陡峭，通常用于高速通信，相反，速率越慢也就意味着信号沿会更加柔和
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