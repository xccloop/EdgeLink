
## 2026-09-14调试笔记

### 提要
昨天是软件理想写完+硬件最终版焊接完毕，今天来一步一步测试底层也就是BSP是否正常

### 1.Edgenode-LED测试
项目的第一步是进行编译与烧录，我们编写了flash.psl进行烧录

初步问题:运行下述命令后发生错误
```bash
    .\edgenode\flash.psl
```
纠错过程

1.报错是 ParserError（意外的 } / 缺少 }）——根因是编码
现已修复：把脚本存成 UTF-8 with BOM。

2.编码修复后遇到
```buash
    mingw32-make.exe : Drivers/BSP/CH340/ch340.c: In function 'fputc':+ FullyQualifiedErrorId : NativeCommandError
```
$ErrorActionPreference = "Stop" 是元凶。 原生程序（make、gcc、openocd）把日志/警告都写 stderr；PowerShell 5.1 
在 Stop 下会把第一行 stderr 当成终止性错误抛出（NativeCommandError），脚本还没走到你写的 if($LASTEXITCODE) 就崩了。
友好错误提示因此根本不会执行。OpenOCD 全程往 stderr 输出，第 4 步一定会中招。
修法：别用 Stop，用 Continue，靠 $LASTEXITCODE 判断成败

3.继续运行发现
```bash
    couldn't open C:UsersmemoryDesktopEdgeLinkedgenodeuildedegnode.elf
```
这是反斜杠的锅，已修复

4.flash.psl编译成功，但是串口无输出，led不亮
错误原因：init_all并没有包含led_init，单独加入后进行编译烧录LED成功点亮

### 2.Edgenode-串口测试

初步问题：调用printf后并没有串口输出

直接使用
```C
usart_data_transmit(USART0, 'A');
```
电脑可以接收到A，判断硬件没有问题，问题出在printf的重定向
猜测：在 GCC + newlib 下，printf 重定向的正确做法是实现 _write，不是 fputc。
```C
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
```C
    printf("\nEdgenode start");
```

### 3.Edgenode-KEY测试
实验：按下对应按键，串口显示哪个按键按下

在EXTI中编写了printf打印，main运行后按下按键没有对应的串口输出，不过有edgenode start，串口没有问题，问题出现在key侧

在KEY初始化函数内部加入pritf打印可以发现keyinit已经被调用，问题出现在EXTI侧
核心原因是因为没有加入board_init进行中断配置初始化
加入后按下按键会显示KEYx press，但是按下一个按键之后会一直发KEYx press

问题找到，原本在中断中只有一句pritf，没有进行中断的标志位清除，导致一直中断中发送printf
我们需要调用完一次中断后清除标志位，修改代码如下
```C
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