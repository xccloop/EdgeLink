#include "CH340/ch340.h"
#include "gd32f10x.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_usart.h"

/*
    只发送版本。对照应用的 Drivers/BSP/CH340/ch340.c：
    那边多了 PA10 的浮空输入配置、usart_receive_config()、接收中断使能，
    以及 printf 的 _write 重定向 —— 那几样都是为了收 ESP-AT 数据，
    Bootloader 用不上，全部砍掉。

    PA9  TX  复用推挽输出（Alternate: USART0_TX）
    PA10 RX  本驱动不配置 —— 不收数据，就让它保持复位后的状态

    这里不做重映射：USART0 默认就在 PA9/PA10 上，
    一旦重映射反而会把这两个脚让给别的功能。
*/

#define CH340_TX_PORT   GPIOA
#define CH340_TX_PIN    GPIO_PIN_9

#define CH340_BAUDRATE  115200

void ch340_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_USART0);

    gpio_init(CH340_TX_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, CH340_TX_PIN);

    usart_deinit(USART0);

    usart_baudrate_set(USART0, CH340_BAUDRATE);
    usart_word_length_set(USART0, USART_WL_8BIT);
    usart_stop_bit_set(USART0, USART_STB_1BIT);
    usart_parity_config(USART0, USART_PM_NONE);
    usart_hardware_flow_rts_config(USART0, USART_RTS_DISABLE);
    usart_hardware_flow_cts_config(USART0, USART_CTS_DISABLE);

    /* 只开发送。不调 usart_receive_config()，也不使能任何中断。 */
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_enable(USART0);
}

void ch340_flush(void)
{
    /*
        TC（Transmission Complete）表示整个发送彻底结束 ——
        数据寄存器和移位寄存器都空了。
        没发过数据时 TC 本来就是置位的，所以这个循环不会误等。
    */
    while (RESET == usart_flag_get(USART0, USART_FLAG_TC))
    {
    }
}

/*
    发送一个字节。

    等的是 TBE（发送数据寄存器空），不是 TC：
    每写一个字节前确认上一个字节已经从数据寄存器搬走了。
    整行发完之后还差"移位寄存器里那一个"没发完，
    所以跳转前要靠 ch340_flush() 补上这一步。
*/
void ch340_put_byte(uint8_t byte)
{
    while (RESET == usart_flag_get(USART0, USART_FLAG_TBE))
    {
    }

    usart_data_transmit(USART0, byte);
}

void ch340_puts(const char *text)
{
    while (*text != '\0')
    {
        ch340_put_byte((uint8_t)(*text));
        text++;
    }
}

void ch340_put_hex32(uint32_t value)
{
    static const char hex_digits[] = "0123456789ABCDEF";
    uint8_t nibble_index;
    uint32_t shift;

    /* 从最高位的半个字节（nibble）开始，一共 8 个。 */
    for (nibble_index = 0U; nibble_index < 8U; nibble_index++)
    {
        shift = 28U - (4U * (uint32_t)nibble_index);

        ch340_put_byte((uint8_t)hex_digits[(value >> shift) & 0x0FU]);
    }
}

void ch340_put_dec(uint32_t value)
{
    /*
        十进制只能"从低位往高位"算出来（先取余、再除），
        但发送必须从高位往低位。所以先存进数组，再倒着发。
        uint32_t 最大 4294967295，共 10 位。
    */
    char digits[10];
    uint8_t count = 0U;

    if (value == 0U)
    {
        ch340_put_byte((uint8_t)'0');
        return;
    }

    while (value > 0U)
    {
        digits[count] = (char)('0' + (value % 10U));
        count++;
        value /= 10U;
    }

    while (count > 0U)
    {
        count--;
        ch340_put_byte((uint8_t)digits[count]);
    }
}
