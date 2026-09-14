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
    rcu_periph_clock_enable(RCU_USART0);

    //这里我们不需要使用重映射，即使PA9,PA10 default为普通io，但是我们使能USART0代表着使用PA9,PA10
    //如果此时我们使用USART0的重映射，也就意味着会把原本的USART0(PA9,PA10)变更为USART0(PB6/PB7)，这反而会导致原本的PA9,PA10没有任何数据接受发送

    //记得吗，我们之前讨论gpio的八种模式，对于tx这种不需要接受数据反而要求要数据可靠性的我们使用推挽输出，同时我们使用复用，这里采用复用推挽
    //而对于RX接受端来说，我们希望它数据可以接近原始所以采用浮空输入
    gpio_init(CH340_TX_PORT, GPIO_MODE_AF_PP,GPIO_OSPEED_50MHZ, CH340_TX_PIN);
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

    //USART0的NVIC优先级由board_config_init统一配置
    // 使能串口接收中断
    usart_interrupt_enable(USART0, USART_INT_RBNE);

}

/*
    关于串口的发送，我们这里使用标准库printf的重定向，即编写wriet函数(GCC + newlib 下，printf 重定向的正确做法是实现 _write，不是 fputc。)，函数内部我们用usart_transmit将数据发送
    第二句话的while是用与确保数据发送完了也就是确定比发送数据的标志位是没有的，然后才return
*/
    int _write(int file, char *ptr, int len)
    {
        (void)file;
        for (int i = 0; i < len; i++)
        {
            /* 先等“发送缓冲空”，再写进去（顺序：wait → transmit） */
            while (RESET == usart_flag_get(USART0, USART_FLAG_TBE))
            {
            }
            usart_data_transmit(USART0, (uint8_t)ptr[i]);
        }
        return len;   /* 必须返回实际写出的字节数，否则 printf 认为失败 */
    }

//使用重定向将串口发送很简单，我们这里为了学习手动实现该如何发送不不同长度的数据
/*
    我们分别来看这四个函数
    第一个函数我们知道串口是逐字节发送，所以我们要发送一个字节直接调用就好了
    第二个是发送一个数组，也比较简单，for循环+逐字节发送就完成了
    第三个是发送一个字符串，C 字符串最后有 '\0'，而在内部，
    每个字符本质上都可以用一个字节的数据表示，因此可以依次调用单字节发送函数将字符发送出去。当读取到 '\0' 时，说明字符串已经结束，此时停止发送
    比较复杂的是最后一个，现在加入我们像发送一个10，最后我们会发现接受的结果却是0X0A，转化为二进制则是0000 1010，你会发现用二进制显示出来正好是10，0X0A正好是十六进制的10
    但是我们想要他发哦是那个10而不用进行如此复杂的换算，但同时我们也发现如果输入的仅仅是0-9，那数字是可以正常显示的，也就有了我们第四个函数的写法
    我们通过取余的操作将一整个数字如123中的每一位提取然后分别发送这样就做到了我们想要实现的效果
*/
/*
    void ch340_send_bit(uint8_t data)
    {   
        usart_data_transmit(USART0,data);
    } 

    bool ch340_send_buffer(uint8_t data*,int length)
    {
        if(data == nullptr || length == 0)
        {
            return false;
        }
        for(int i = 0;i < length;i++)
        {
            ch340_send_bit(data[i]);
        }
        return true;
    }

    void ch340_send_string(const char *str)
    {
        while (*str != '\0')
        {
            ch340_send_byte((uint8_t)*str);
            str++;
        }
    }

    void ch340_send_uint32(uint32_t num)
    {
        char buffer[10];
        uint8_t index = 0;

        if (num == 0)
        {
            ch340_send_byte('0');
            return;
        }

        while (num > 0)
        {
            buffer[index++] = '0' + num % 10;
            num /= 10;
        }

        while (index > 0)
        {
            ch340_send_bit(buffer[--index]);
        }
    }
*/
