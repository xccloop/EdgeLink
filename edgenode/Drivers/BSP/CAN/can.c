#include "can.h"
#include "gd32f10x.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_can.h"

/*
    这个文件我们呢来编写CAN相关，通讯相关的底层我们都要实现传输+接受，我们用到的引脚是，PB8,PB9
    这里顺便说一下，在硬件上我们并不是将PB8,PB9直接连接到输出端子用于发送接受CAN信号，因为CAN是差分电平数据
    因此我们要使用一个类似于中转站的SN65HVD来进行，我们称PB8,PB9位CAN控制，SN65HVD为CAN收发

    PB8 61 I/O 5VT 
    Default: PB8 
    Alternate: TIMER3_CH2(6), SDIO_D4(4), TIMER9_CH0(3) 
    Remap: I2C0_SCL, CAN0_RX 
    PB9 62 I/O 5VT 
    Default: PB9 
    Alternate: TIMER3_CH3(6), SDIO_D5(4), TIMER10_CH0(3) 
    Remap: I2C0_SDA, CAN0_TX
*/

#define CAN_RX_PORT GPIOB
#define CAN_RX_PIN GPIO_PIN_8
#define CAN_TX_PORT GPIOB
#define CAN_TX_PIN GPIO_PIN_9

/*
    之前CAN初始化失败时，Can_init会直接返回，但发送函数不知道这件事，仍可能继续访问CAN外设。
    用这个标记记录初始化是否完整成功；发送前先检查它，避免把“没有准备好的CAN”当成可以发送。
*/
static uint8_t can0_is_initialized = 0U;

void Can_init()
{
    can0_is_initialized = 0U;
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_CAN0);

    gpio_pin_remap_config(GPIO_CAN_PARTIAL_REMAP, ENABLE);//记得要重映射不然CAN0对应的不是PB8,PB9
    gpio_init(CAN_TX_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, CAN_TX_PIN);
    gpio_init(CAN_RX_PORT,GPIO_MODE_IN_FLOATING,GPIO_OSPEED_50MHZ,CAN_RX_PIN);

    /*
        CAN是我唯一没有特别接触学习过的通信协议，在此，我们来学习何为CAN
        CAN 全称是（Controller Area Network），控制器局域网络，是ISO国际标准化的串行通信协议，CAN是国际上应用最广泛的现场总线之一。
        CAN通信只有两根信号线，分别是CAN_High和CAN_Low，CAN 控制器根据这两根线上的电位差来判断总线电平。总线申平分为显性电平和隐性申平，二者必居其一。
        发送方通过使总线电平发生变化，将消息发送给接收方。
        CAN网络也采用总线制，在CANH,CANL上可以链接多个外设
        之前我们在edgehub上面初步使用过用socketCAN，他大部分调用系统库进行编程，但是核心思想可以学习
        CAN是的帧是可以进行自定义的，分为基础帧（11位）--CAN2.0A，拓展帧（29位)--CAN2.0B，二者的区别在于can_id的不同
        剩下的我们便配置变边细说
    */
    /*
        这里我们开始配置CAN的内部参数。CAN和USART有一点不一样，USART是我们直接写115200
        来确定波特率，但是CAN需要我们自己把“一位数据需要多少时间”算出来。

        CAN把一位数据切成很多很小的时间块，我们把每一个小时间块叫做TQ，Time Quantum，
        翻译过来就是时间量子。可以简单理解成：CAN通信中的一位不是一次完成的，
        TQ 是 CAN 内部用来切分一个 bit 时间的最小时间单位，多个 TQ 组成一个 bit，CAN 在这个 bit 的某个 TQ 位置进行采样。
        而是由很多个小格子拼出来的。那为什么要把一位拆成这么多小格子？
        实际的工程中，物理电平并非完美，因此一个数据比如1可能在脉宽75%的时候才稳定，因此我们考虑真实情况以后决定用一个采样点来选择是在脉宽的哪个位置进行读取电平
        主要是因为CAN总线上的信号传输不是瞬间完成的。数据从一个节点发送出去，
        还需要经过CAN控制器、CAN收发器、总线，再到另外一个节点，所以中间会存在一定的传播延迟。
        如果接收方一看到这一位开始，就立刻去读取当前电平，那么此时信号可能还没有完全稳定，
        就有可能把这一位的数据读取错误。
        所以CAN不会在一位刚开始的时候就直接读取数据，而是会先等待一段时间，然后在这一位中的某一个固定位置进行采样，这个位置就叫做采样点。
        我们规在Time_Segment_1之后为采样点，因此我们配置这两个的时候不能只考虑波特率是否计算正确，还要考虑采样点是否合理
        一位CAN数据通常可以分成几个部分：
        Sync_Seg + Time_Segment_1 + Time_Segment_2
        要注意的是CAN是差分信号，因此硬件侧也就是SN65HVD会在采样点同时读取CANH,CANL然后相减得出差分电压，判断是0 or 1
        CAN 收发器根据 CANH 和 CANL 之间的差分电压判断总线状态，具体阈值和内部判决方式由收发器规格决定。

        这一位最开始固定有1个小格子，后面再加上time_segment_1和time_segment_2。
        prescaler就是把CAN时钟放慢多少倍。三个参数合在一起，才决定我们最后是500K还是250K。
        波特率 = PCLK1 / [prescaler × (1 + time_segment_1 + time_segment_2)]。（后续对应外设时钟线挂载都可以在Usermannul的Page 32查找
        这里的PCLK1代表着就是APB1的时钟
        working_mode是设置CAN现在要怎么工作。正常模式就是我们真正连接到CANH、CANL总线中通信；
        回环模式是芯片自己发送、自己接收，用来测试程序；静默模式是只听总线上的数据，不发送数据。
        我们真正和其他板子通信的时候选择正常模式。
        auto_bus_off_recovery里面的Bus-Off可以理解成：CAN发现自己连续出错太多次，
        为了不一直干扰总线，它会先让自己断开总线。这个参数决定它之后要不要自动重新加入总线。
        auto_retrans中的retrans是retransmission的缩写，就是重新发送。CAN发送数据时，
        如果发生错误，或者别的节点的ID更小、优先级更高，硬件要不要自动帮我们重新发送这一帧。
        auto_wake_up中的wake_up就是唤醒。CAN进入睡眠状态以后，如果总线上有新的数据，
        开启它就可以让CAN自己醒来。我们现在还没有做低功耗，可以先关闭。
        rec_fifo_overwrite里面FIFO可以理解成一个排队等我们处理的接收区，先来的数据排在前面。
        如果这个队列满了，开启overwrite，也就是覆盖，就让新数据挤掉最早的数据；关闭则保留最早的
        数据，丢掉新来的数据。以后要看我们更在意最新状态还是完整记录。
        FIFO是先进先出的意思，后续的一些数据类型的学习都会遇到
        time_triggered就是按照严格规定的时间点发送数据。普通传感器通信不需要这个功能，所以关闭。
        time_segment_1是一个数据位中，采样数据之前有多少个小格子。信号在线上跑也需要时间，
        这个参数就是给信号跑到其他节点、稳定下来留出时间。它越大，采样数据的时间点越靠后。
        time_segment_2是采样完数据到这一位结束之间还有多少个小格子。它和time_segment_1
        一起决定一位总共有多少个小格子。
        resync_jump_width中的resync就是重新同步。不同板子的时钟不可能完全一样，CAN发现两边的
        节奏有一点偏差时，会调整自己的节奏跟上总线。这个参数就是一次最多允许调整几个小格子。
        trans_fifo_order中的trans是发送，三个发送邮箱都排着数据时，这个参数决定先发哪一个。
        可以按照谁先放进去谁先发，也可以让ID更小、优先级更高的数据先发。
        prescaler就是CAN时钟分频。它的意思是CAN每隔多少个原始时钟，才走一个上面说的小格子。
        所以我们设置它之前，必须先确认CAN使用的PCLK1到底是多少MHz。
    */
    can_parameter_struct can_init_handler;
    can_struct_para_init(CAN_INIT_STRUCT, &can_init_handler);//清空结构体
    can_init_handler.working_mode =  CAN_NORMAL_MODE;//注意，这个宏才是给can_init的
    can_init_handler.auto_bus_off_recovery = ENABLE;
    can_init_handler.auto_retrans = ENABLE;
    can_init_handler.auto_wake_up = DISABLE;
    can_init_handler.rec_fifo_overwrite = ENABLE;
    can_init_handler.trans_fifo_order = ENABLE;
    can_init_handler.resync_jump_width = CAN_BT_SJW_1TQ;
    can_init_handler.time_triggered = DISABLE;
    can_init_handler.time_segment_1 = CAN_BT_BS1_8TQ;
    can_init_handler.time_segment_2 = CAN_BT_BS2_3TQ;//这里要注意先手顺序，我们的
    can_init_handler.prescaler = 6;
    //baudrate = 36 / (6 * (1 + 8 + 3)) = 500Kbit/s

    if(can_init(CAN0,&can_init_handler) != SUCCESS)
    {
        return;
    }

    can_filter_parameter_struct can_filter_handler;
    can_struct_para_init(CAN_FILTER_STRUCT, &can_filter_handler);
    can_filter_handler.filter_enable = ENABLE;
    can_filter_init(&can_filter_handler);

    //现在我们配置中断相关用于接受数据的
    //这里配置的是启用FIFO接受区不为空的中断使能
    /*
        CAN0的FIFO0接收中断和USBD低优先级中断共用同一个NVIC向量，名字因此是
        USBD_LP_CAN0_RX0_IRQn，对应的中断函数也是USBD_LP_CAN0_RX0_IRQHandler。
        这不代表CAN数据进入USB；只是两个外设共用同一条中断线。现在我们没有启用USB，
        进入该中断函数的原因就是CAN0的FIFO0收到数据。
        如果以后同时启用USB和CAN，就要在同一个中断函数中分别检查USB和CAN的中断标志。
    */
    can_interrupt_enable(CAN0, CAN_INT_RFNE0);
    can0_is_initialized = 1U;
}

/*
    这个函数只负责发送一帧CAN标准数据帧，不规定data里面每一个字节具体代表什么。
    standard_id是11位CAN ID，data是我们想发送的原始数据，data_length是实际发送多少字节，最大8字节。
    函数返回值是硬件分配的发送邮箱编号0、1、2；返回CAN_NOMAILBOX代表CAN没有初始化完成、参数不正确，或者三个发送邮箱都没有空位。
    注意：拿到邮箱编号只代表数据已经交给CAN硬件等待发送，不代表另一端已经收到数据。
*/
uint8_t can0_data_send(uint16_t standard_id, const uint8_t *data, uint8_t data_length)
{
    uint8_t i;
    can_trasnmit_message_struct can_transmit_message;

    if ((can0_is_initialized == 0U) || (standard_id > CAN_SFID_MASK) || (data_length > 8U) || ((data == 0) && (data_length != 0U))) {
        return CAN_NOMAILBOX;
    }

    can_struct_para_init(CAN_TX_MESSAGE_STRUCT, &can_transmit_message);
    can_transmit_message.tx_sfid = standard_id;
    can_transmit_message.tx_ff = CAN_FF_STANDARD;
    can_transmit_message.tx_ft = CAN_FT_DATA;
    can_transmit_message.tx_dlen = data_length;

    for (i = 0U; i < data_length; i++) {
        can_transmit_message.tx_data[i] = data[i];
    }

    return can_message_transmit(CAN0, &can_transmit_message);
}

