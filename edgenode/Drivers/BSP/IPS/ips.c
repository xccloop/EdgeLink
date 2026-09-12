#include "ips.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_spi.h"
#include "gd32f10x_dma.h"
#include "board_time.h"

/*
    现在我们来编写最为复杂的IPS，IPS硬件为240*320的ST7789
    | PB3 | SCL / SCK  
    | PB5 | SDA / MOSI 
    | PC4 | CS         
    | PC5 | DC         
    | PC8 | RST        
    | PC9 | BLK        

    PB3 55 I/O 5VT 
    Default: JTDO 
    Alternate:SPI2_SCK(4), I2S2_CK(4) 
    Remap: PB3, TRACESWO(4), TIMER1_CH1, SPI0_SCK
    PB5 57 I/O  
    Default: PB5 
    Alternate: I2C0_SMBA, SPI2_MOSI(4), I2S2_SD(4) 
    Remap: TIMER2_CH1, SPI0_MOSI 
    PC4 24 I/O  Default: PC4 
    Alternate: ADC01_IN14 
    PC5 25 I/O  Default: PC5 
    Alternate: ADC01_IN15
    PC8 39 I/O 5VT 
    Default: PC8 
    Alternate: TIMER7_CH2(4), SDIO_D0(4) 
    Remap: TIMER2_CH2 
    PC9 40 I/O 5VT 
    Default: PC9 
    Alternate: TIMER7_CH3(4), SDIO_D1(4) 
    Remap: TIMER2_CH3 
*/

/*
    这个文件把ST7789 IPS屏的刷屏过程封装成BSP层，APP只需要调用ips_init()完成一次性硬件初始化，再反复调用ips_flush_line()逐行提交RGB565像素数据，
    不需要直接碰SPI、DMA、CS/DC或ST7789命令；底层使用SPI2的PB3(SCK)、PB5(MOSI)以及PC4(CS)、PC5(DC)、PC8(RST)、PC9(BLK)，因为240×320的RGB565全屏有153600字节，
    若让CPU逐字节写SPI会长期占用CPU，所以用DMA1_CH1把APP准备好的行缓冲区自动搬到SPI2数据寄存器，SPI2再通过PB3/PB5发给屏幕。静态变量ips_initialized防止重复初始化
    ，ips_dma_active表示DMA是否正在占用APP缓冲区，ips_dma_deadline_ms用于传输超时判断。ips_dma_stop()负责关闭DMA1_CH1和SPI2的DMA发送请求；
    ips_dma_abort()在超时或DMA错误时统一收尾，清除标志、释放active并拉高CS；ips_spi_wait_tbe()等待SPI发送缓冲区为空，ips_spi_wait_idle()等待SPI移位寄存器真正空闲，
    二者都带超时；ips_write_command_data()在一次CS有效期内先拉低DC发命令、再拉高DC发参数，最后等SPI空闲并拉高CS，所有ST7789命令都通过它发送。
    ips_init()打开GPIOB、GPIOC、SPI2、DMA1时钟，把PB3/PB5配成复用推挽，把CS/DC/RST/BLK配成推挽输出并先设安全电平
    ，配置SPI2为Mode0、主机、只发送、8bit、9MHz、软件NSS，配置DMA1_CH1为内存到外设、内存地址递增、8bit宽度、外设地址固定为SPI_DATA(SPI2)、高优先级，
    然后复位ST7789并依次发送软件复位、退出睡眠、RGB565、横屏扫描、打开显示等命令，最后打开背光并置ips_initialized。ips_dma_start()在每次刷行时被调用，
    它检查初始化状态、data指针、byte_count、DMA空闲状态，并确认整段源数据落在GD32F103RCT6的48KB SRAM范围内，同时确认SPI2不在传输，
    然后关闭DMA通道、重设内存地址和传输数量、清除旧标志、拉低CS、拉高DC、使能SPI2的DMA发送请求和DMA1_CH1，并记录超时时间。ips_dma_poll()用于轮询本次DMA是否完成，
    若active为0则返回IDLE，若超时或DMA错误则调用ips_dma_abort()并返回ERROR，若DMA完成标志未置位则返回BUSY，若DMA完成则先关闭DMA和SPI的DMA请求，
    再等待SPI2的TRANS清零，只有SPI真正空闲后才清除标志、释放active、拉高CS并返回COMPLETE。ips_memory_write_begin()在DMA空闲时设置一行窗口，先检查坐标和宽高是否越界
    ，再计算x_end/y_end，通过0x2A设置列地址、0x2B设置行地址、0x2C准备写内存。ips_flush_line()是开放给APP的唯一刷行接口，它先调用ips_dma_poll()检查上一行是否完成，
    若BUSY则返回IPS_FLUSH_BUSY且本次data不提交，若ERROR则返回IPS_FLUSH_ERROR，若data为NULL则只确认上一行并返回IPS_FLUSH_COMPLETE，
    若data非空则调用ips_memory_write_begin()设置一行窗口并调用ips_dma_start()启动DMA，成功返回IPS_FLUSH_ACCEPTED表示本行数据已被DMA接管，APP不能立即改写该缓冲区。
    整个流程就是：ips_init()一次性初始化硬件和屏幕，ips_flush_line()逐行提交数据，ips_memory_write_begin()告诉屏幕写哪里，
    ips_dma_start()启动DMA搬运像素，ips_dma_poll()检查完成、超时和错误，ips_write_command_data()负责所有命令和参数，从而让APP只管理自己的显示缓冲区，
    而SPI2、DMA1_CH1、CS/DC、窗口命令和异常恢复全部由IPS BSP封装。
*/

/*
    一次 DMA 传输的完整生命周期
    以刷一行 640 字节为例：
    把行缓冲区地址写到 DMA 的内存地址寄存器，把 640 写到传输数量寄存器。
    你使能 SPI2 的 DMA 发送请求，再使能 DMA1_CH1。
    SPI2 发送缓冲区空，发出第一个请求。
    DMA 从内存读第 1 个字节，写到 SPI2 数据寄存器，内存地址加 1，数量从 640 变成 639。
    SPI2 把这个字节移出去，缓冲区又空，再发请求。
    DMA 搬第 2 个字节……如此重复。
    当数量减到 0 时，DMA 硬件把“全传输完成标志”置位，这就是 DMA_FLAG_FTF。
    如果开了中断，这时会触发中断；用的是轮询，就在 ips_dma_poll() 里查这个标志。
*/

#define IPS_SCK_PORT GPIOB
#define IPS_SCK_PIN GPIO_PIN_3
#define IPS_MOSI_PORT GPIOB
#define IPS_MOSI_PIN GPIO_PIN_5
#define IPS_CS_PORT GPIOC
#define IPS_CS_PIN GPIO_PIN_4
#define IPS_DC_PORT GPIOC
#define IPS_DC_PIN GPIO_PIN_5
#define IPS_RST_PORT GPIOC
#define IPS_RST_PIN GPIO_PIN_8
#define IPS_BLK_PORT GPIOC
#define IPS_BLK_PIN GPIO_PIN_9
#define IPS_SPI_TIMEOUT 100000U
#define IPS_DMA_TIMEOUT_MS 20U
#define IPS_SRAM_START 0x20000000U
#define IPS_SRAM_END 0x2000BFFFU

/* 这个标志只属于BSP，用来防止APP在上一段数据尚未发送完时再次改写DMA寄存器。 */
static uint8_t ips_dma_active;
static uint8_t ips_initialized;
static uint32_t ips_dma_deadline_ms;

typedef enum
{
    IPS_DMA_STATE_IDLE = 0U,
    IPS_DMA_STATE_BUSY,
    IPS_DMA_STATE_COMPLETE,
    IPS_DMA_STATE_ERROR
} ips_dma_state_enum;

static void ips_dma_stop(void)
{
    /* 先关闭DMA通道，再关闭SPI2的DMA发送请求，避免DMA在配置过程中继续搬运数据。 */
    dma_channel_disable(DMA1, DMA_CH1);
    spi_dma_disable(SPI2, SPI_DMA_TRANSMIT);
}

static void ips_dma_abort(void)
{
    /* DMA或SPI异常时统一收尾，确保APP不会继续把这块缓冲区当作DMA占用。 */
    ips_dma_stop();
    dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_G);
    ips_dma_active = 0U;
    gpio_bit_set(IPS_CS_PORT, IPS_CS_PIN);
}

static uint8_t ips_spi_wait_tbe(void)
{
    uint32_t timeout = IPS_SPI_TIMEOUT;

    while(spi_i2s_flag_get(SPI2, SPI_FLAG_TBE) == RESET)
    {
        if(timeout-- == 0U)
        {
            return IPS_FAIL;
        }
    }
    return IPS_SUCCESS;
}

static uint8_t ips_spi_wait_idle(void)
{
    uint32_t timeout = IPS_SPI_TIMEOUT;

    while(spi_i2s_flag_get(SPI2, SPI_FLAG_TRANS) != RESET)
    {
        if(timeout-- == 0U)
        {
            return IPS_FAIL;
        }
    }
    return IPS_SUCCESS;
}

static uint8_t ips_write_command_data(uint8_t command, const uint8_t *data, uint8_t length)
{
    uint8_t i;
    uint8_t success = IPS_SUCCESS;

    /* 一条命令和它的参数必须在同一次CS有效期间连续发送，DC低表示命令、DC高表示参数。 */
    gpio_bit_reset(IPS_CS_PORT, IPS_CS_PIN);
    gpio_bit_reset(IPS_DC_PORT, IPS_DC_PIN);

    if(ips_spi_wait_tbe() == IPS_FAIL)
    {
        success = IPS_FAIL;
    }
    else
    {
        spi_i2s_data_transmit(SPI2, command);
        gpio_bit_set(IPS_DC_PORT, IPS_DC_PIN);

        for(i = 0U; i < length; i++)
        {
            if(ips_spi_wait_tbe() == IPS_FAIL)
            {
                success = IPS_FAIL;
                break;
            }
            spi_i2s_data_transmit(SPI2, data[i]);
        }
    }

    if(ips_spi_wait_idle() == IPS_FAIL)
    {
        success = IPS_FAIL;
    }
    gpio_bit_set(IPS_CS_PORT, IPS_CS_PIN);
    return success;
}

uint8_t ips_init(void)
{
    const uint8_t pixel_format = 0x55U;
    const uint8_t memory_access_control = 0xA0U;

    /* IPS初始化会重置SPI、DMA和屏幕，DMA正在读取APP缓冲区时绝对不能重复执行。 */
    if((ips_dma_active != 0U) || (ips_initialized != 0U))
    {
        return ips_initialized;
    }

    /* SPI2负责通过PB3、PB5发送屏幕数据；GPIOB、GPIOC负责全部屏幕引脚。 */
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_SPI2);
    rcu_periph_clock_enable(RCU_DMA1);

    /* PB3默认是JTAG的JTDO。board_config_init已经关闭JTAG、保留SWD，所以这里可以作为SPI2时钟。 */
    gpio_init(IPS_SCK_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, IPS_SCK_PIN);
    gpio_init(IPS_MOSI_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, IPS_MOSI_PIN);

    /* 先写好安全的默认电平，再切换成输出，避免刚初始化时误选中屏幕或点亮背光。 */
    gpio_bit_set(IPS_CS_PORT, IPS_CS_PIN);
    gpio_bit_set(IPS_DC_PORT, IPS_DC_PIN);
    gpio_bit_set(IPS_RST_PORT, IPS_RST_PIN);
    gpio_bit_reset(IPS_BLK_PORT, IPS_BLK_PIN);

    //推挽
    gpio_init(IPS_CS_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, IPS_CS_PIN);
    gpio_init(IPS_DC_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, IPS_DC_PIN);
    gpio_init(IPS_RST_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, IPS_RST_PIN);
    gpio_init(IPS_BLK_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, IPS_BLK_PIN);

    //其实我们发现我们只用到了SPI的两根线，是因为IPS不需要向单片机返回数据，不用IIC是因为SPI的传输速度很快
    //接下来我们配置SPI
    /*
        具体是干什么的就不细说了，这里主要说一下为什么这样选择
    */
    spi_parameter_struct spi_init_handler;
    spi_struct_para_init(&spi_init_handler);
    spi_init_handler.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE; /* Mode 0：空闲低，SDA 在上升沿采样 */
    spi_init_handler.device_mode          = SPI_MASTER;
    spi_init_handler.trans_mode           = SPI_TRANSMODE_BDTRANSMIT; /* 屏幕只接 MOSI，不读回 */
    spi_init_handler.endian               = SPI_ENDIAN_MSB;
    spi_init_handler.nss                  = SPI_NSS_SOFT;             /* CS 由 PC4 手动控制 */
    spi_init_handler.prescale             = SPI_PSC_4;                /* SPI2 时钟 36MHz / 4 = 9MHz */
    spi_init_handler.frame_size           = SPI_FRAMESIZE_8BIT;
    spi_init(SPI2, &spi_init_handler);
    spi_enable(SPI2);

    /*
        这里介绍一下DMA
        在我们BMP280的通信中，我们发现，其实数据是需要CPU一个字节一个字节地写入SPI数据寄存器，
        再等待发送完成后才能继续发送下一个字节。BMP280一次读取的数据并不多，这样做问题不大，
        但是IPS屏幕一次刷新的数据量就完全不同了。
        例如我们使用RGB565格式时，一个像素需要16bit也就是2个字节，240*320的整屏数据就有153600个字节。
        如果仍然让CPU负责每个字节的发送和等待，CPU大部分时间都停留在发送像素数据这件事情上，
        后续按键、传感器、串口等功能就没有足够的时间去执行。
        DMA可以理解为单片机内部一个专门搬运数据的模块。这里我们只需要先告诉DMA：
        源地址是内存中准备好的像素数据，目标地址是SPI2的数据寄存器，传输方向是内存到外设，
        然后由SPI2每次需要发送下一个字节时向DMA发出请求，DMA就自动把下一个字节搬到SPI2。
        这样SPI仍然负责把数据从PB5发给IPS，而CPU只负责开始一次传输和等待传输结束。
        GD32F103中SPI2的发送请求固定对应DMA1通道1，所以后续配置DMA时需要打开DMA1时钟，并配置DMA1通道1。
        像素缓冲区的地址需要自增，因为每次都要取下一个像素字节；SPI2数据寄存器的地址不能自增，
        因为所有数据最终都要写到同一个SPI2数据寄存器。
        还需要注意DMA传输完成只代表最后一个字节已经搬到SPI2数据寄存器，
        并不代表最后8个时钟已经从PB3、PB5发送完毕。因此在CS拉高结束一次写屏前，
        仍然需要等待SPI2真正空闲，否则最后一个字节可能没有完整发送到IPS。
    */
    dma_parameter_struct dma_init_handler;

    /*
        SPI2的发送请求固定使用DMA1通道1，所以这里配置的是DMA1的通道1。
        DMA的参数和SPI类似，也是先把我们需要的工作方式写入结构体，
        最后再通过dma_init()统一写进DMA的寄存器。

        需要注意的是，一次DMA传输的数据地址和传输长度不是固定的。
        例如后续画不同的位置、不同大小的图片时，像素数据缓冲区的起始地址和字节数都会变化。
        因此这里先完成不随每次发送变化的基础配置，memory_addr和number先设置为0，
        真正开始一次刷屏时，再在发送函数里重新填写这两个参数并启动DMA。
    */
    dma_deinit(DMA1, DMA_CH1);
    dma_struct_para_init(&dma_init_handler);

    /*
        direction表示DMA搬运数据的方向。IPS的像素数据先存放在单片机RAM中，
        需要送到SPI2的数据寄存器，因此选择从内存到外设。
        memory_addr表示本次要发送的像素数据在RAM中的起始地址。
        现在还没有准备具体的像素缓冲区，所以先填写0；不能把0当作真正发送地址使用。
        后续开始发送时，会把它替换成像素数组或者一行像素缓冲区的地址。
        memory_inc表示DMA每搬运完一个数据后，内存地址是否自动加1。
        我们要连续读取像素缓冲区中的每一个字节，所以内存地址必须递增。
        memory_width表示每次从内存取出多少位数据。SPI当前使用8bit帧，
        RGB565的16bit像素也会拆成两个8bit数据依次发送，因此这里选择8bit。
        periph_width表示每次写入外设寄存器多少位数据。SPI2的数据寄存器当前按8bit发送，
        所以这里也必须选择8bit，才能和SPI的frame_size保持一致。
        periph_addr表示DMA搬运数据的目标外设地址。SPI_DATA(SPI2)就是SPI2的数据寄存器，
        DMA每次收到SPI2的发送请求后，都会把一个像素字节写到这个固定地址中。
        periph_inc表示DMA每次写完数据后，外设地址是否自动加1。
        SPI2只有一个数据寄存器，所有发送数据都必须写入同一个地址，因此不能递增。
        number表示本次DMA需要搬运多少个数据。因为现在还没有确定本次发送的像素缓冲区，
        所以先设置为0；后续发送函数会按照实际字节数重新设置，横屏一行320个RGB565像素就是640。
        priority表示DMA仲裁时的优先级。刷屏数据量较大，如果优先级过低可能被其他DMA请求频繁打断，
        所以这里选择高优先级；这只是DMA内部的仲裁顺序，并不等于CPU中断优先级。
        到这里DMA1通道1的基础属性已经配置完成，但它还没有像素地址和发送长度，
        也没有打开SPI2的DMA发送请求，所以不会自行开始传输。
    */
    dma_init_handler.direction = DMA_MEMORY_TO_PERIPHERAL;
    dma_init_handler.memory_addr = 0U;
    dma_init_handler.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    dma_init_handler.memory_width = DMA_MEMORY_WIDTH_8BIT;
    dma_init_handler.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    dma_init_handler.periph_addr = (uint32_t)&SPI_DATA(SPI2);
    dma_init_handler.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    dma_init_handler.number = 0U;
    dma_init_handler.priority = DMA_PRIORITY_HIGH;
    dma_init(DMA1, DMA_CH1, &dma_init_handler);

    /*
        ST7789上电后还不能直接写像素，先复位并选择RGB565和横屏扫描方向。
        下面的命令是控制器通用的基础初始化，具体伽马、电压等面板相关参数暂不在这里猜测。
    */
    gpio_bit_reset(IPS_RST_PORT, IPS_RST_PIN);
    delay_ms(10U);
    gpio_bit_set(IPS_RST_PORT, IPS_RST_PIN);
    delay_ms(120U);

    if(ips_write_command_data(0x01U, 0, 0U) == IPS_FAIL) /* 软件复位 */
    {
        gpio_bit_reset(IPS_BLK_PORT, IPS_BLK_PIN);
        return IPS_FAIL;
    }
    delay_ms(120U);
    if(ips_write_command_data(0x11U, 0, 0U) == IPS_FAIL) /* 退出睡眠 */
    {
        gpio_bit_reset(IPS_BLK_PORT, IPS_BLK_PIN);
        return IPS_FAIL;
    }
    delay_ms(120U);

    if((ips_write_command_data(0x3AU, &pixel_format, 1U) == IPS_FAIL) || /* RGB565 */
       (ips_write_command_data(0x36U, &memory_access_control, 1U) == IPS_FAIL) || /* 横屏 */
       (ips_write_command_data(0x29U, 0, 0U) == IPS_FAIL)) /* 打开显示 */
    {
        gpio_bit_reset(IPS_BLK_PORT, IPS_BLK_PIN);
        return IPS_FAIL;
    }

    gpio_bit_set(IPS_BLK_PORT, IPS_BLK_PIN);
    ips_initialized = 1U;
    return IPS_SUCCESS;
}

/*
    到这里ips_init()的工作已经结束：GPIO、SPI2、DMA和ST7789都已经准备好，
    但是初始化并不能直接完成显示，因为它不知道APP下一次要画什么内容。

    后续显示过程内部仍然需要把三件事情拆开处理：
    1. ips_memory_write_begin()先告诉ST7789，这批像素要写到哪个X/Y窗口；
    2. ips_dma_start()再让DMA从APP准备好的缓冲区中搬运实际像素字节；
    3. ips_dma_poll()最后确认DMA和SPI是否已经发送完成。

    但这三个函数只是IPS BSP内部实现一次刷行所需的步骤，不再直接开放给APP。
    APP只需要调用ips_flush_line()提交一行数据；IPS BSP负责把“位置、数据、传输状态”
    安全地转换成实际硬件动作。这样APP只管理自己的缓冲区，不需要直接碰SPI和DMA寄存器。
*/
static uint8_t ips_dma_start(const uint8_t *data, uint16_t byte_count)
{
    uint32_t data_start = (uint32_t)data;
    uint32_t data_end;

    /*
        data和byte_count虽然由APP层传入，但APP层不会直接修改DMA寄存器。
        这样可以保证每次修改地址和数量前，DMA通道一定已经关闭。
    */
    if((ips_initialized == 0U) || (data == 0) || (byte_count == 0U) ||
       (byte_count > IPS_DMA_LINE_BYTES) || (ips_dma_active != 0U))
    {
        return IPS_FAIL;
    }

    /* DMA不会检查指针是否有效，必须由BSP确认整段源数据都位于GD32F103RCT6的48KB SRAM中。 */
    data_end = data_start + (uint32_t)byte_count - 1U;
    if((data_start < IPS_SRAM_START) || (data_end < data_start) || (data_end > IPS_SRAM_END))
    {
        return IPS_FAIL;
    }

    /* 如果SPI2本身还在发送上一笔数据，也不能让新的DMA传输插入进去。 */
    if(spi_i2s_flag_get(SPI2, SPI_FLAG_TRANS) != RESET)
    {
        return IPS_FAIL;
    }

    ips_dma_stop();

    /*
        这两个值每次传输都会不同：内存地址指向APP已经填好的那一行像素，
        number表示这一行要发送多少个8bit数据。这里DMA配置为8bit，因此640字节就是640个数据。
    */
    dma_memory_address_config(DMA1, DMA_CH1, (uint32_t)data);
    dma_transfer_number_config(DMA1, DMA_CH1, byte_count);

    /* 清除上一笔传输遗留的完成、半完成和错误状态，避免把旧状态误当成本次已经完成。 */
    dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_G);

    /* SPI2产生发送请求后，DMA才会把内存中的下一个字节自动写入SPI2数据寄存器。 */
    gpio_bit_reset(IPS_CS_PORT, IPS_CS_PIN);
    gpio_bit_set(IPS_DC_PORT, IPS_DC_PIN);
    spi_dma_enable(SPI2, SPI_DMA_TRANSMIT);
    dma_channel_enable(DMA1, DMA_CH1);
    ips_dma_active = 1U;
    ips_dma_deadline_ms = board_systick_ms + IPS_DMA_TIMEOUT_MS;
    return IPS_SUCCESS;
}

static ips_dma_state_enum ips_dma_poll(void)
{
    if(ips_dma_active == 0U)
    {
        return IPS_DMA_STATE_IDLE;
    }

    /* 无论DMA请求丢失还是SPI移位寄存器异常，超过合理时间后都要释放通道，不能永久占住APP缓冲区。 */
    if((uint32_t)(board_systick_ms - ips_dma_deadline_ms) < 0x80000000U)
    {
        ips_dma_abort();
        return IPS_DMA_STATE_ERROR;
    }

    /* DMA错误时必须先停掉通道，APP随后才能释放对应的行缓冲区。 */
    if(dma_flag_get(DMA1, DMA_CH1, DMA_FLAG_ERR) != RESET)
    {
        ips_dma_abort();
        return IPS_DMA_STATE_ERROR;
    }

    if(dma_flag_get(DMA1, DMA_CH1, DMA_FLAG_FTF) == RESET)
    {
        return IPS_DMA_STATE_BUSY;
    }

    /*
        DMA完成只表示最后一个字节已经写入SPI2数据寄存器，所以先关闭DMA请求，
        再检查SPI2的移位过程是否真正结束。只有TRANS清零后，APP才可以复用这行缓冲区。
    */
    ips_dma_stop();
    if(spi_i2s_flag_get(SPI2, SPI_FLAG_TRANS) != RESET)
    {
        return IPS_DMA_STATE_BUSY;
    }

    dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_G);
    ips_dma_active = 0U;
    gpio_bit_set(IPS_CS_PORT, IPS_CS_PIN);
    return IPS_DMA_STATE_COMPLETE;
}

static uint8_t ips_memory_write_begin(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    uint16_t x_end;
    uint16_t y_end;
    uint8_t column_data[4];
    uint8_t row_data[4];

    /* 这次接口只允许在DMA空闲时设置窗口，避免命令插入正在发送的像素数据中。 */
    if((ips_initialized == 0U) || (ips_dma_active != 0U) || (width == 0U) || (height == 0U) ||
       (x >= IPS_WIDTH) || (y >= IPS_HEIGHT) ||
       (width > (IPS_WIDTH - x)) || (height > (IPS_HEIGHT - y)))
    {
        return IPS_FAIL;
    }

    x_end = (uint16_t)(x + width - 1U);
    y_end = (uint16_t)(y + height - 1U);

    column_data[0] = (uint8_t)(x >> 8U);
    column_data[1] = (uint8_t)x;
    column_data[2] = (uint8_t)(x_end >> 8U);
    column_data[3] = (uint8_t)x_end;

    row_data[0] = (uint8_t)(y >> 8U);
    row_data[1] = (uint8_t)y;
    row_data[2] = (uint8_t)(y_end >> 8U);
    row_data[3] = (uint8_t)y_end;

    if((ips_write_command_data(0x2AU, column_data, 4U) == IPS_FAIL) ||
       (ips_write_command_data(0x2BU, row_data, 4U) == IPS_FAIL) ||
       (ips_write_command_data(0x2CU, 0, 0U) == IPS_FAIL))
    {
        return IPS_FAIL;
    }

    return IPS_SUCCESS;
}

ips_flush_result_enum ips_flush_line(const uint8_t *data, uint16_t x, uint16_t y, uint16_t width)
{
    ips_dma_state_enum dma_state;

    /*
        APP每提交新的一行前，先由IPS检查上一行的DMA是否已经结束。
        data为NULL时只做这一步，用于APP没有新行时确认最后一行是否已经完成。
        返回ACCEPTED时，当前data开始被DMA使用；返回BUSY时，本次data根本没有交给DMA。
    */
    dma_state = ips_dma_poll();
    if(dma_state == IPS_DMA_STATE_BUSY)
    {
        return IPS_FLUSH_BUSY;
    }
    if(dma_state == IPS_DMA_STATE_ERROR)
    {
        return IPS_FLUSH_ERROR;
    }

    if(data == 0)
    {
        return IPS_FLUSH_COMPLETE;
    }

    /* 一行像素的目标位置和数据传输必须连续完成，避免APP遗漏设置窗口或RAMWR命令。 */
    if((ips_memory_write_begin(x, y, width, 1U) == IPS_FAIL) ||
       (ips_dma_start(data, (uint16_t)(width * IPS_RGB565_BYTES_PER_PIXEL)) == IPS_FAIL))
    {
        return IPS_FLUSH_ERROR;
    }

    return IPS_FLUSH_ACCEPTED;
}
