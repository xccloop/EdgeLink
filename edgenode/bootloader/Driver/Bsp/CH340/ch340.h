#ifndef CH340_H_
#define CH340_H_

#include <stdint.h>

/*
    调试串口 USART0（PA9 = TX）的【只发送】驱动。

    和应用的 Drivers/BSP/CH340 是同一个硬件，但用途不同：
    Bootloader 只往串口打日志，从不接收，所以这里砍掉了：
      - RX 引脚（PA10）的配置
      - usart_receive_config()
      - 接收中断 usart_interrupt_enable(USART0, USART_INT_RBNE)

    应用那份要收 ESP-AT 的数据，所以它需要这些；Bootloader 不需要。

    跳转前为什么必须调 ch340_flush()：
      每个字节发出前只等 TBE（发送数据寄存器空），函数返回时
      最后一个字节可能还卡在移位寄存器里没发出去。
      应用启动后会 usart_deinit(USART0) 重新配置串口，
      那一刻未发完的字节就被截断了 —— 最后一行日志会少字符或乱码。
*/
void ch340_init(void);

/* 等最后一次发送彻底完成（TC 标志）。跳转进应用之前必须调用。 */
void ch340_flush(void);

/*
    最小格式化输出 —— 用来替代 printf。

    为什么不用 printf：newlib 的 printf 会拖进约 2.5KB 的 stdio 机制
    （格式化内核、输出缓冲、以及 malloc/free）。Bootloader 区只有 16KB，
    而且要留给以后可能加的 IPS 显示，付不起这个开销；
    Bootloader 里出现动态内存也不是好味道。

    这三个函数加起来不到 100 字节，够打日志用了。
*/
void ch340_put_byte(uint8_t byte);

/* 输出以 '\0' 结尾的字符串。 */
void ch340_puts(const char *text);

/* 输出 32 位十六进制，固定 8 位带前导零。例：0x8004000 -> "08004000" */
void ch340_put_hex32(uint32_t value);

/* 输出无符号十进制。例：0 -> "0"，1234 -> "1234" */
void ch340_put_dec(uint32_t value);

#endif
