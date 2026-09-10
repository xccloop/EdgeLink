#include "adc.h"
#include "board_time.h"
#include "gd32f10x.h"
#include "gd32f10x_adc.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"

/*
    这个文件我们来实现ADC采样，在板卡中ADC采样时监测12V电池电量的手段，我们采用的是PA1

    PA1 15 I/O
    Default: PA1
    Alternate: USART1_RTS, ADC012_IN1(5), TIMER1_CH1, 
    TIMER4_CH1(4)

*/
#define ADC_PORT GPIOA
#define ADC_PIN GPIO_PIN_1

/*
    这里我们顺带复习一下什么叫做ADC,即模数传化，我们常说的电压如电池电压等电压我们叫做模拟电路，在模拟电路中，电压是连续的
    而单片机内部产生的高低电平位逻辑电平，虽然高电平是3.3V但不代表这所有的高电平都是3.3V，我们用01表示是为了在数学上有更好的计算表现
    单片机内部要求我们使用逻辑电压，但是外部却是模拟电压，因此我们要监测电压直接连接是不可行的，这就是ADC存在的意义，
    ADC在模拟信号转化为数字信号需要经过采样、保持、量化和编码。采样和保持在采样保持电路中完成，而量化和编码步骤则在ADC中完成。
    采样是指ADC在一定时间间隔内对连续变化的模拟信号进行取样，实现在有限采样率条件下，无失真还原信号波形信息。取样的频率决定了每秒采集的样本量，接下里我们详细说明ADC流程

    首先我们要配置一个 ADC 的通道，就相当于告诉 ADC 具体要采集哪一路模拟输入。
    然后我们要选择 ADC 的触发方式，触发的作用是告诉 ADC“现在开始一次采样和转换”，而不是通过标志位触发。
    对于我这种要监测电池电量的情况，可以选择上电后定期触发一次 ADC，例如每隔一段时间采样一次。
    然后就到了具体的采样过程。从用户层理解，输入电压是一个持续变化的过程，但在 ADC 内部，这个电压会进入采样保持电路，给采样电容充电。
    单片机实际进行转换的是采样电容上保持的电压。这里就引出了采样时间，本质上就是设置一个时间，让采样电容在这段时间内充电到足够接近输入电压。
    采样时间结束后，输入与采样电容断开，此时采样电容保持采样结束前的电压。我们设置 ADC 为 x 位，也就是共有 2^x 个量化状态，数字编码范围为 0 到 2^x-1，
    相当于把 0~3.3V 这个区间划分成很多离散区间。
    之前我们得到了采样电容上的电压，ADC 硬件会判断这个电压落在哪一个量化区间，并得到对应的数字码，可以近似理解为：
    ADC_Value ≈ (V / 3.3) × (2^x - 1)
    这个结果不是简单地向上取整，而是由 ADC 硬件量化得到对应的数字编码。
    最后，ADC 硬件会自动把转换得到的数字结果写入 ADC 数据寄存器，并设置转换完成标志位。后续 CPU 就可以读取 ADC 数据寄存器，得到这次采样后的数字值。
*/
void adc_init()
{
    /*
        再开始写ADC之前我们先去datasheet找到我们想要的信息，我们定位到 ADC characteristics(For GD32F103xC/D/E/F/G/I/K devices)，我们是RC是符合这个的
        我们可以看到fADCmin = 0.6,fADCmax = 14(mhz)注意我们这里不是要配置系统时钟是14mhz而是配置adc的时钟，在GD32F103xx clock tree章节我们可以分析出
        外部晶振起振后，通分频等操作变成108MHZ到CK_AHB中，然后再进入到APB2中，这里显示APB2的分频是不固定的要自己配置，然后再分频进入到ADC中
    */
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_ADC0);
    //关于时钟分频这种会影响全局的我们统一放在board_config中管理

    //这里设置gpio为模拟输入，因为是读取电压嘛
    gpio_init(ADC_PORT,GPIO_MODE_AIN ,GPIO_OSPEED_50MHZ, ADC_PIN);

    //现在我们来配置ADC独有的配置
    //这里配置的是ADC模式,相关的宏存在于/* ADC sync mode */，我们可以跳转过去查看其注释，这里的意思是所有ADC独立工作，因为我们只用到一个ADC所以不需要很复杂
    adc_mode_config(ADC_MODE_FREE);

    //这一行用于设置连续转化模式（我们是单通道），配置好以后代表转化一次ADC以后继续转化，如果是多通道可以采用扫描模式（ADC_SCAN_MODE）
    //这里的函数是开启ADC的特别功能而不是特指转化和扫描,但是我们的ADC服务于电池电量监测，因此无需使用连续而是一会儿来一次
    adc_special_function_config(ADC0, ADC_CONTINUOUS_MODE ,DISABLE);

    //这里配置的是最终的ADC的值是存储在寄存器的什么位置，我们查看ADC characteristics的Sampling rate的一栏发现，我们使用的ADC均为12位
    //但是ADC的数据存储寄存器有16位（详情见GD32F10x0_usermannul -- 数据寄存器  (ADC_RDATA)）因此我们要选择讲述存储在那里的十二位
    //从右边开始数12位就是右对齐，反之左对齐，我们这里配置右对齐
    adc_data_alignment_config(ADC0, ADC_DATAALIGN_RIGHT);

    //这里配置的是ADC的外部触发源，我们不需要，所以舍去，值得一提的是我们可以选择其余事件比如EXTI作为ADC的触发
    //第一个是ADC0，第二个是所有通道，第三个是ADC012都关闭，详情见/* for ADC0 and ADC1 regular channel */
    //第二个函数同样是配置外部触发源，我们这里选择开启是因为外部事件source关闭，但是我们可以自己手动创建事件这就软件进行ADC的触发，可以保留
    adc_external_trigger_source_config(ADC0,ADC_REGULAR_CHANNEL,ADC0_1_2_EXTTRIG_REGULAR_NONE);
    adc_external_trigger_config(ADC0, ADC_REGULAR_CHANNEL, ENABLE);

    //这里配置的是规则通道序列的长度，我们现在只需要采集PA1对应的ADC通道1，因此这一轮规则转换里面只需要放1个通道
    //注意这里最后的1不是ADC_CHANNEL_1，而是代表规则转换序列里面一共有1个转换位置
    adc_channel_length_config(ADC0, ADC_REGULAR_CHANNEL, 1);

    //这里开始配置这个唯一的规则通道，第一个参数代表我们使用ADC0，第二个参数0代表这个通道排在规则转换序列的第0位
    //第三个参数ADC_CHANNEL_1才是真正代表采集ADC通道1，也就是PA1对应的ADC输入
    //最后一个参数ADC_SAMPLETIME_55POINT5代表这个通道的采样时间为55.5个ADC时钟周期，用来给内部采样电容留出足够的充电时间
    adc_regular_channel_config(ADC0,0,ADC_CHANNEL_1,ADC_SAMPLETIME_55POINT5);

    //最后是使能ADC
    adc_enable(ADC0);
    delay_ms(1);
    //这个函数用于使能ADC校准
    adc_calibration_enable(ADC0);
}

uint16_t adc_data_get()
{
    //这里使用软件触发开始一次规则通道的ADC转换，因为我们前面没有使用定时器或者EXTI等外部事件进行触发
    adc_software_trigger_enable(ADC0, ADC_REGULAR_CHANNEL);

    //这里等待ADC转换完成，ADC_FLAG_EOC代表End Of Conversion，也就是规则通道转换完成标志位
    //当EOC还没有被置位时就在这里等待，等ADC硬件完成采样和转换以后才继续往下执行
    while (RESET == adc_flag_get(ADC0, ADC_FLAG_EOC))
    {
    }

    //这里直接读取ADC规则通道的数据寄存器并返回原始值
    //GD32F103的ADC为12位，因此正常情况下返回值范围为0~4095
    return (uint16_t)adc_regular_data_read(ADC0);
}