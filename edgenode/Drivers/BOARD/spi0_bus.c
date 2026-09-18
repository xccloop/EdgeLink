#include "spi0_bus.h"
#include "board_time.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_spi.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define SPI0_BUS_TIMEOUT_MS 10U
#define SPI0_BUS_MAX_POLLS 100000U

static uint8_t spi0_bus_faulted;
static StaticSemaphore_t spi0_bus_mutex_storage;
static SemaphoreHandle_t spi0_bus_mutex;

/*
    由于BMP280,W25Q32使用同一总线，为了避免重复初始化，这里我们移动到单独文件
*/

void spi0_bus_init(void)
{
    spi_parameter_struct spi_init_handler;

    /* 运行期重配 SPI0 会打断正在进行的 CS 事务，因此恢复必须通过未来专用流程完成。 */
    if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        return;
    }

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
        第一个参数就是我们先前介绍的SPI四种模式中其一对应的是模式0,这里选择模式0同时是因为BMP280与GD25Q32仅支持模式0和模式3（详情见数据手册的SPI Interfernce）
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

    /* 启动阶段创建静态 mutex；任务开始后由它序列化两颗 SPI 从设备。 */
    if(spi0_bus_mutex_init() == 0U)
    {
        spi0_bus_faulted = 1U;
        return;
    }

    spi0_bus_faulted = 0U;
}

uint8_t spi0_bus_mutex_init(void)
{
    if(spi0_bus_mutex != NULL)
    {
        return 1U;
    }

    spi0_bus_mutex = xSemaphoreCreateMutexStatic(&spi0_bus_mutex_storage);
    return (spi0_bus_mutex != NULL) ? 1U : 0U;
}

uint8_t spi0_bus_lock(void)
{
    if(spi0_bus_mutex == NULL)
    {
        return 0U;
    }

    /* 初始化阶段还没有任务并发访问 SPI0，不应在调度器启动前阻塞。 */
    if(xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
    {
        return 1U;
    }

    return (xSemaphoreTake(spi0_bus_mutex, portMAX_DELAY) == pdTRUE) ? 1U : 0U;
}

void spi0_bus_unlock(void)
{
    if((spi0_bus_mutex != NULL) &&
       (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING))
    {
        (void)xSemaphoreGive(spi0_bus_mutex);
    }
}

/*
    我们在这里做SPI0的基础建设
    我们知道SPI位全双工通信，这也就意味着当主设备向从设备发送一个字节的时候，同样会得到从设备向主设备发送的一个字节，因此这个函数的逻辑我们就确定了
*/
static void spi0_receive_cleanup(void)
{
    /*
        修改：上一次接收超时后，迟到字节可能仍留在接收寄存器。
        旧代码不清理它，下一笔传输会把旧字节误当作新的从机回复，导致寄存器数据错位。
    */
    while (spi_i2s_flag_get(SPI0, SPI_FLAG_RBNE) == SET) {
        (void)spi_i2s_data_receive(SPI0);
    }
    (void)SPI_STAT(SPI0);
}

static uint8_t spi0_wait_flag(uint32_t flag, FlagStatus expected)
{
    uint32_t start = board_systick_ms;
    uint32_t polls = SPI0_BUS_MAX_POLLS;

    while (spi_i2s_flag_get(SPI0, flag) != expected) {
        /*
            修改：旧代码无限等待硬件标志。接线、供电或SPI异常时CPU会永久停在这里，
            主循环中的ESP、CAN、RS485、显示等后续代码都无法执行。
        */
        if ((board_systick_ms - start) >= SPI0_BUS_TIMEOUT_MS) {
            return 0U;
        }
        /*
            修改：若SysTick尚未启动、全局中断关闭或在中断上下文调用，旧的毫秒超时不会前进。
            轮询次数上限不依赖中断，仍能让函数有限时间返回失败。
        */
        if (polls == 0U) {
            return 0U;
        }
        polls--;
    }
    return 1U;
}

uint8_t spi0_tansfer_data(uint8_t send_data, uint8_t *receive_data)
{
    if ((receive_data == 0) || (spi0_bus_faulted != 0U)) {
        /*
            修改：若上一笔传输直到TRANS超时，SPI状态已不可信。
            旧代码仍会继续选中下一颗设备，它可能接收残余时钟；现在拒绝后续传输，
            本版不提供运行期恢复；必须复位后重新初始化 SPI0。
        */
        return 0U;
    }

    spi0_receive_cleanup();

    if (spi0_wait_flag(SPI_FLAG_TBE, SET) == 0U) {
        return 0U;
    }
    spi_i2s_data_transmit(SPI0, send_data);

    /*
        修改：旧接口只能返回收到的字节，因此真数据0x00和超时返回0x00无法区分。
        新接口以返回值表示成功/失败，并经receive_data指针带回真实接收字节。
    */
    /*
        我们之前配置了SPI0一次发送8一次传输单位就是 8 bit，即 1 byte。SPI 每来一个时钟沿传一位数据，所以这里是8个时钟
    */
    if (spi0_wait_flag(SPI_FLAG_RBNE, SET) == 0U) {
        spi0_bus_wait_idle();
        spi0_receive_cleanup();
        return 0U;
    }
    *receive_data = (uint8_t)spi_i2s_data_receive(SPI0);
    return 1U;
}

uint8_t spi0_bus_wait_idle(void)
{
    uint8_t idle;

    /*
        修改：在CS拉高结束一笔事务前确认最后一个字节已发完。
        RBNE只说明接收寄存器已有数据；TRANS清零才说明SPI不再输出时钟。
    */
    idle = spi0_wait_flag(SPI_FLAG_TRANS, RESET);
    if (idle == 0U) {
        /* 结束CS前仍应释放当前从设备，但本次启动内不再允许任何后续SPI事务。 */
        spi0_bus_faulted = 1U;
    }
    return idle;
}
