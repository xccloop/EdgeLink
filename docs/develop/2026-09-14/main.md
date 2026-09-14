
# 2026-09-14 调试笔记

## 提要

昨天是软件理想写完+硬件最终版焊接完毕，今天来一步一步测试底层也就是BSP是否正常

## 1. Edgenode-LED 测试

项目的第一步是进行编译与烧录，我们编写了flash.psl进行烧录

### 初步问题

运行下述命令后发生错误

```bash
    .\edgenode\flash.psl
```

### 纠错过程

1. 报错是 ParserError（意外的 } / 缺少 }）——根因是编码
现已修复：把脚本存成 UTF-8 with BOM。

2. 编码修复后遇到

```buash
    mingw32-make.exe : Drivers/BSP/CH340/ch340.c: In function 'fputc':+ FullyQualifiedErrorId : NativeCommandError
```

$ErrorActionPreference = "Stop" 是元凶。 原生程序（make、gcc、openocd）把日志/警告都写 stderr；PowerShell 5.1 
在 Stop 下会把第一行 stderr 当成终止性错误抛出（NativeCommandError），脚本还没走到你写的 if($LASTEXITCODE) 就崩了。
友好错误提示因此根本不会执行。OpenOCD 全程往 stderr 输出，第 4 步一定会中招。
修法：别用 Stop，用 Continue，靠 $LASTEXITCODE 判断成败

3. 继续运行发现

```bash
    couldn't open C:UsersmemoryDesktopEdgeLinkedgenodeuildedegnode.elf
```

这是反斜杠的锅，已修复

4. flash.psl编译成功，但是串口无输出，led不亮
错误原因：init_all并没有包含led_init，单独加入后进行编译烧录LED成功点亮

## 2. Edgenode-串口测试

初步问题：调用printf后并没有串口输出

直接使用

```c
usart_data_transmit(USART0, 'A');
```

电脑可以接收到A，判断硬件没有问题，问题出在printf的重定向
猜测：在 GCC + newlib 下，printf 重定向的正确做法是实现 _write，不是 fputc。

```c
    //这是替代原本fput的
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

    //只加 _write 还不够——stdout 默认带缓冲，printf("...") 没有 \n 就不会刷新；而且缓冲要 malloc，nosys 下 _sbrk 是空桩 → 分配可能直接失败。
    //在 main() 里、初始化之后加一行：
    setvbuf(stdout, NULL, _IONBF, 0);   /* 关掉缓冲：每个字节立刻经 _write 发出 */
```
修正后串口可以正常接受，但是没有分行

开头加入换行符号之后正常显示

```c
    printf("\nEdgenode start");
```

## 3. Edgenode-KEY 测试

实验：按下对应按键，串口显示哪个按键按下

在EXTI中编写了printf打印，main运行后按下按键没有对应的串口输出，不过有edgenode start，串口没有问题，问题出现在key侧

在KEY初始化函数内部加入pritf打印可以发现keyinit已经被调用，问题出现在EXTI侧
核心原因是因为没有加入board_init进行中断配置初始化
加入后按下按键会显示KEYx press，但是按下一个按键之后会一直发KEYx press

问题找到，原本在中断中只有一句pritf，没有进行中断的标志位清除，导致一直中断中发送printf
我们需要调用完一次中断后清除标志位，修改代码如下

```c
    void EXTI1_IRQHandler()
    {
        //这里采用exti_flag_get函数来获取EXTI1此时的标志位，虽然已经进入中断了，但是以防万一我们还是再判断一次
        //这里的返回值位：typedef enum {RESET = 0, SET = !RESET} FlagStatus;
        if(exti_flag_get(EXTI_1) == SET)
        {
            printf("KEY1 press\n");
            exti_interrupt_flag_clear(EXTI_1);
        }
    }
```

现在3个按键中断为1，8，15对应KEY1 2 3,而我们想要的对应关系是3 2 1
这个简单，修改一下引脚对应标号就好了

## 3. Edgenode-BMP280 测试

实验：串口输出BMP280采集到的温度

### 问题

```c
    while(1)
    {
        float temperature = bmp280_temperature_get();
        printf("%f",temperature);
    }
```
烧录后串口无输出

```c
    /* 修改：chip_id是本驱动的第一层运行证据；不是0x58就不继续读校准或温度。 */
    if (bmp280_data_get(BMP280_REG_ID, &chip_id) == 0U) {
        /* 按你当前代码的语义：0 表示读取失败 */
        printf("read bmp280 chip id failed\r\n");
    } else if (chip_id != BMP280_CHIP_ID) {
        printf("error bmp chip id is 0x%02X\r\n", (unsigned)chip_id);
    }
    printf("bmp chip id is %x",chip_id);
```
printf打印chip id输出发现读取错误

```bash
    Edgenode start
    read bmp280 chip id failed
    bmp chip id is 0
```

又遇到问题，重新上电后 st-link烧录程序失败

```bash
[步骤 4/4] 通过 ST-Link SWD 烧录并校验
Open On-Chip Debugger 0.12.0 (2023-01-14-23:37)
Licensed under GNU GPL v2
For bug reports, read
        http://openocd.org/doc/doxygen/bugs.html
Info : The selected transport took over low-level target control. The results might differ compared to plain JTAG/SWD
none separate
System.Management.Automation.RemoteException
Info : clock speed 1000 kHz
Info : STLINK V2J46S7 (API v2) VID:PID 0483:3748
Info : Target voltage: 3.243114
Error: init mode failed (unable to connect to the target)
in procedure 'program'
** OpenOCD init failed **
shutdown command invoked
System.Management.Automation.RemoteException
```
可能是复位的问题，我尝试按住复位键然后进行烧录，等openocd开是烧录的时候松开结果烧录成功

回到BMP280测试，问题出现在board_init没有在main的开头进行调用，导致后续的时钟等没有起作用
调换位置后打印chip_id正常显示58，说明bmp280被识别到了并且SPI工作正常
但是并没有输出温度,问题出在了温度的获取上
在温度获取函数的末尾加入打印发现确实有输出，问题出在打印函数的内部，而不是有没有被调用
再次打印原始温度获取值

```c
        printf("temp_raw = %lu (0x%05lX)\r\n",
       (unsigned long)temperature_raw,
       (unsigned long)temperature_raw);
```
串口有输出，在53348左右，查看数据手册后发现值是正常的
难道问题出在了计算函数上面吗？
加入打印后发现计算函数也被成功调用
问题出现在打印浮点数上？
猜测，我们没有加入转浮点数的工具链因此并不能将浮点数打印
但是对于BSP层来说，他不应该直接计算出float的温度，他应该是将温度+缩放转移给上层才对
当前代码中的message段会进行缩放计算，这是不对的
架构更新：BSP层返回温度加缩放，collection层补充最后的采集时间

```c
typedef struct
{
    int32_t temperature;
    int8_t temperature_scale;
} bmp280_temperature_t;

/* 这个函数是我们使用BMP280的主要功能，用固定小数结果交给上层。 */
uint8_t bmp280_temperature_get(bmp280_temperature_t *temperature)
{
    uint8_t temperature_data[3];
    uint32_t temperature_raw;

    /* 修改：未通过bmp280_init()或输出地址无效时，不能伪造一笔温度结果。 */
    if ((temperature == 0) || (bmp280_is_initialized == 0U)) {
        return BMP280_FAIL;
    }

    /* 修改：0xFA、0xFB、0xFC必须作为一笔连续读取，避免在CS低时重复发读命令。 */
    if (bmp280_reg_data_get(BMP280_REG_TEMP_MSB, temperature_data, 3U) == 0U) {
        /* 修改：旧代码在传输失败后仍会拼接残缺数据并返回伪温度。 */
        return BMP280_FAIL;
    }

    /* 修改：XLSB只有bit7..4有效，右移4位后才是adc_T的bit3..0。 */
    temperature_raw = ((uint32_t)temperature_data[0] << 12)
                    | ((uint32_t)temperature_data[1] << 4)
                    | ((uint32_t)temperature_data[2] >> 4);
    temperature->temperature = bmp280_temperature_centidegree_calculate(&bmp280_calib, temperature_raw);
    temperature->temperature_scale = BMP280_TEMPERATURE_SCALE;
    return BMP280_SUCCESS;
}
```

修改后调用打印正常显示温度加缩放

```bash
temperature=2676, scale=-2
temperature=2676, scale=-2
temperature=2676, scale=-2
```

## 5. Edgenode-GD25Q32 测试

实验：可以正常返回GD25Q32的值并且实现读取擦除写入

```c
    uint32_t chip_id = ((uint32_t)chip_id_raw[0] << 16U) |
                       ((uint32_t)chip_id_raw[1] << 8U)  |
                       (uint32_t)chip_id_raw[2];
    if(chip_id != 0xC84016)
    {
        printf("chip_id_raw = %02X %02X %02X\r\n",
        chip_id_raw[0], chip_id_raw[1], chip_id_raw[2]);
        return 0U;
    }
```
打印chipid显示00 00 00
GD25,BMP280挂载到同一条SPI上，之前测试了BMP已经导通，那么问题排除SPI损坏
难道是之前测试BMP280的时候让BMP_CS拉低了，复位之后还是拉低，所以没有选中吗？
排除，先拉高BMP_CS在进行初始化，依旧读取00 00 00
猜测：00 00 00给人的感觉像是没有把数据给传输过去或者传输回来，但是之前测试了SPI链路是正常的，或许是FLASH和pin之间的链接有问题
将FLASH芯片引脚进行补焊再次烧录，成功输出chip_id

```bash
chip_id_raw = C8 40 16
chip_id = 0xC84016 (13123606)
```
接下来进行擦除，写入，读取测试

又遇到ST-link识别失败的问题，这下确认是ST-link本身损坏
原因：
st-link插入后明显供电不稳似乎有短路迹象导致插入3V3,GND之后指示灯闪烁而不是常亮
插入后电脑显示的USB供电连接也也会一直掉线-连接-掉线 循环，应该是st-link内部的短路导致板卡也被弄成了短路
板卡本身可以正常工作，只有插上ST-Link才会失败

## 总结

那没办法了，等ST-link到了之后在进行开发吧
