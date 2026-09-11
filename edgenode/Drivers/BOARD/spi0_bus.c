#include "spi0_bus.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_spi.h"

/*
    由于BMP280,W25Q32使用同一总线，为了避免重复初始化，这里我们移动到单独文件
*/

void spi0_bus_init(void)
{
    spi_parameter_struct spi_init_handler;

    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_SPI0);

    gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_5);
    gpio_init(GPIOA, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, GPIO_PIN_6);
    gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_7);

    //首先是时钟分频在对应的章节中（ADC章节有提到过）可以看到fSCKmax = 27，并且在usaermannul的APB2 复位寄存器找到了SPI0的复位寄存器位，我们判定，SPI0挂载于APB2
    /*
        所以接下来需要通过 spi_parameter_struct 这个结构体，
        把 SPI 的工作参数配置好，再调用 spi_init() 将这些参数写入 SPI0 的相关寄存器。
        spi_parameter_struct spi_init_handler;
        这个结构体可以理解为："我们希望 SPI0 以什么方式工作" 的一组配置参数。
        配置完成后：spi_init(SPI0, &spi_init_handler);
        这个函数会读取 spi_init_handler 中的配置，并将对应的参数写入 SPI0 的控制寄存器。
        但是 spi_init() 只是完成配置，最后还需要spi_enable(SPI0)才会真正使能 SPI0 外设。

        现在我们来着重介绍一下spi的初始化所用到的结构体
        第一个参数就是我们先前介绍的SPI四种模式中其一对应的是模式0,这里选择模式0同时是因为BMP280仅支持模式0和模式3（详情见数据手册的SPI Interfernce）
        第二个参数是设置SPI的主从模式，我们知道我们是主设备所以设置为主
        第三个参数是SPI的发送模式，我们这里设置的是双向通信，其他参数在对应部分可以查看
        第四个参数是发送的数据是从高到低发还是从低到高发，设置对应的模式也要照顾从设备的接受端考虑，这和不同操作系统的大小端很类似
        第五个参数是设置一次发送的数据帧长度，这里选择8bit
        第六个参数是设置cs片选是硬件片选还是软件，我们是软件
        第七个参数是设置分频，这一点在前面的介绍中已经提到，不过多赘述

        这些是 PA5/PA6/PA7 共享总线的参数，不属于某一个从设备；后续 BMP280
        和 W25Q32 都复用此处配置，只各自控制自己的 CS。
    */
    spi_struct_para_init(&spi_init_handler);
    spi_init_handler.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE;
    spi_init_handler.device_mode = SPI_MASTER;
    spi_init_handler.trans_mode = SPI_TRANSMODE_FULLDUPLEX;
    spi_init_handler.endian = SPI_ENDIAN_MSB;
    spi_init_handler.frame_size = SPI_FRAMESIZE_8BIT;
    spi_init_handler.nss = SPI_NSS_SOFT;
    spi_init_handler.prescale = SPI_PSC_16;
    spi_init(SPI0, &spi_init_handler);
    spi_enable(SPI0);
}

/*
    我们在这里做SPI0的基础建设
    我们知道SPI位全双工通信，这也就意味着当主设备向从设备发送一个字节的时候，同样会得到从设备向主设备发送的一个字节，因此这个函数的逻辑我们就确定了
*/
uint8_t spi0_tansfer_data(uint8_t data)
{
    /* 等待发送寄存器空，才能写入本次要发送的数据。 */
    while (spi_i2s_flag_get(SPI0, SPI_FLAG_TBE) == RESET) {
    }
    spi_i2s_data_transmit(SPI0, data);

    /* 发送数据产生8个时钟后，等待从设备回传的字节进入接收寄存器。 */
    /*
        我们之前配置了SPI0一次发送8一次传输单位就是 8 bit，即 1 byte。SPI 每来一个时钟沿传一位数据，所以这里是8个时钟
    */
    while (spi_i2s_flag_get(SPI0, SPI_FLAG_RBNE) == RESET) {
    }
    return (uint8_t)spi_i2s_data_receive(SPI0);
}

void spi0_bus_wait_idle(void)
{
    /*
        修改：在CS拉高结束一笔事务前确认最后一个字节已发完。
        RBNE只说明接收寄存器已有数据；TRANS清零才说明SPI不再输出时钟。
    */
    while (spi_i2s_flag_get(SPI0, SPI_FLAG_TRANS) == SET) {
    }
}
