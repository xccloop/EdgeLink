2026-09-18

今天先梳理一下freertos的流程
1.获取Freertos源码并根据开发环境选择对应的protable
2.将1freertos加入makefile编译
3.配置FreeRTOSConfig
4.如果是静态创建任务，那就需要手动编写vApplicationGetIdleTaskMemory，配置TCB,stack
5.编写任务进行测试

在今天开始工作之前，有一个严重的业务问题我们一直没有解决
现在有这么一个情况，transmit层发送失败，数据还在采集，但是已经传输不到hub端了，这该怎么办
换句话说如何在数据恢复通信的时候将之前没有发送成功的数据发送出去
很容易我们就联想到现有的帧协议重的sequence，他是发送的序列号，我们可不可以考虑将他作为数据的传输成功标志呢？
和很可惜，也不行，因为我们只能确保sequence在本node中是递增的，我们可以确保他传输到esp12s，但是没有办法确定他把数据成功发送

我们分析过后发现这其实算是一个很巨大的工程对于当前项目
首先，node端只有transmit而没有receive，这意味着我们要写数据接收协议解析
然后，hub端只有receive而没有transmit，这意味着需要增加ACK，来确保数据已经正确传输，每正确存储一个数据，就发送一个ACK
然后，现有的sequence是uint16，在1s发送一次的情况下，他的回绕时间不过18小时，这显然不是我们希望的，我们需要修改他为uint32
然后，既然修改了sequence的大小，那么node端的message，storage，frame的协议内容字节分布又要修改，不用说hub端的协议解析也要修改
然后，我们做这么多实际上是为了让数据再重启的时候可以发送没有发送的数据，也就是说我们希望他在上电后先进行TCP/CAN数据联通到稳定性，扫描flash中没有发送的数据将数据进行发送然后发送
这只是主干，实际上这个的复杂度有点超出我的想象了
我们还要考虑：
ACK丢了怎么办
Hub已落库但Node没收到ACK怎么办
TCP超时后CAN发送时TCP ACK迟到怎么办
Flash写一半断电怎么办
pending状态修改一半断电怎么办
多个pending按什么顺序恢复
恢复一直失败是否阻塞正常采集
队列满了怎么办
谁有权操作Flash
谁维护next_sequence


初步完成后启动项目
```bash
KEY init finsh
chip_id_raw = C8 40 16
chip_id = 0xC84016 (13123606)
Edgenode start
chip_id_raw = C8 40 16
chip_id = 0xC84016 (13123606)
```
没有反应，等了一会发现是可能是storage的扫描太耗费时间了
我们发现当前的storage存储策略具有大问题为了避免断电后续写到可疑槽位，重启后放弃整个“部分使用过的扇区”。相当浪费空间
并且由于它是按照


## FreeRTOS任务运行一段时间后卡死

### 现象

在完成sequence持久化保存并开始发送后，运行一段时间出现以下现象：

```text
TCP不再输出
LED1和LED2都保持高电平不再闪烁
```

一开始我以为是刚加入的TCP ACK等待、Flash扫描或者SPI0共享导致了卡死。因为StorageTask和TransmitTask的优先级为3，两个LED任务优先级为2，如果高优先级任务一直运行，低优先级的LED任务确实可能没有机会执行。

### 猜测与排除

1. 猜测：TransmitTask等待TCP ACK和CAN ACK时使用while循环空转，导致优先级为2的LED任务被优先级为3的TransmitTask饿死。

   验证方法：在两个LED都保持高电平时，不复位开发板，通过ST-Link + OpenOCD连接SWD，暂停CPU并读取当前PC和调用栈。

   结果：现场PC没有停在`tcp_ack_wait()`、`can_ack_wait()`或其他`tcp_wait_*()`中，因此它不是这次完全卡死的直接原因。

   但是检查代码后确认这些等待函数确实存在`continue`空转，后续仍需要改为任务阻塞等待，不能把它当作已经解决的问题。

2. 猜测：`board_systick_ms`不再递增，导致所有依赖超时的while循环永远无法退出。

   验证方法：暂停现场后读取`board_systick_ms`，并结合当前PC判断是否在超时循环中。

   结果：GDB读到`board_systick_ms = 0x6230b`，并且PC不在TCP/CAN超时循环，而是在FreeRTOS的栈溢出Hook中。本次现象不是由SysTick停止直接导致。

3. 猜测：任务栈溢出，FreeRTOS进入`vApplicationStackOverflowHook()`后关闭中断并死循环。

   验证方法：使用当前编译出的`build/edegnode.elf`连接正在卡死的开发板：

```bash
arm-none-eabi-gdb build/edegnode.elf
target remote :3333
monitor halt
info registers pc lr sp
p/x board_systick_ms
bt
x/10i $pc
```

   实际GDB现场：

```text
0x080001fc in vPortRaiseBASEPRI () at ThirdParty/FreeRTOS-Kernel/portable/GCC/ARM_CM3/portmacro.h:216
pc             0x80001fc
lr             0x80032f9
sp             0x2000bfc8
$1 = 0x6230b
#0  0x080001fc in vPortRaiseBASEPRI ()
#1  vApplicationStackOverflowHook (task=0x20000dc8 <led2_task_tcb>,
    task_name=0x20000dfc <led2_task_tcb+52> "led2")
    at User/App/FreeRtos/rtos_hooks.c:100
#2  0x080032f8 in vTaskSwitchContext ()
#3  0x08003d4e in PendSV_Handler ()
=> 0x80001fc <vApplicationStackOverflowHook+16>: b.n 0x80001fc
```

   结果：确认触发栈溢出的任务是`led2`。`vApplicationStackOverflowHook()`在第100行调用`taskDISABLE_INTERRUPTS()`，随后在while(1)中停住，因此会同时看到TCP停止、两个LED保持最后的电平。

### 根因

`led2_task_stack`虽然声明为：

```c
static StackType_t led2_task_stack[256];
```

它实际分配了256个`StackType_t`。在Cortex-M3中一个`StackType_t`是4字节，因此这个数组本身有1024字节。

但是创建任务时传给`xTaskCreateStatic()`的`ulStackDepth`却写成了`32U`：

```c
xTaskCreateStatic(led2_task, "led2", 32U, NULL, 2U,
                  led2_task_stack, &led2_task_tcb)
```

FreeRTOS依据第三个参数决定该任务可使用的栈深度。也就是说，虽然数组准备了256个字，LED任务实际只允许使用32个字，也就是128字节。

同时当前配置：

```c
#define configMINIMAL_STACK_SIZE 128
#define configCHECK_FOR_STACK_OVERFLOW 2
```

LED任务传入的32个字甚至小于空闲任务使用的最小栈深度128个字。运行一段时间后，LED2任务的栈边界被检测到破坏，FreeRTOS正确调用了栈溢出Hook。Hook的设计就是关闭中断并停机，所以表现为整个系统看起来卡死。

### 修改计划

本次排查先不直接修改代码。下一步需要把LED任务的`xTaskCreateStatic()`第三个参数改为与对应数组一致的`256U`，使FreeRTOS实际使用256个`StackType_t`，而不是只使用32个。

这里要特别注意：数组大小和传入`xTaskCreateStatic()`的栈深度必须一致；只把数组写大，但仍传入小的`ulStackDepth`没有意义。

同时还需要单独处理TransmitTask中TCP/CAN ACK的空转等待。它不是本次死机的根因，但在网络不通、发送队列持续有数据时仍可能让低优先级任务长期得不到调度。

### 验证结果

- [x] 源码检查：确认LED栈数组为256个字，但创建参数为32个字。
- [x] SWD现场：未复位开发板，成功暂停到故障现场。
- [x] FreeRTOS运行时证据：调用栈明确指出`vApplicationStackOverflowHook(task="led2")`。
- [ ] 修复后构建：尚未修改。
- [ ] 修复后烧录：尚未进行。
- [ ] 修复后运行：尚未进行。
- [ ] TCP/ACK持续发送验证：尚未进行。

### 当前结论

这次“LED全高、TCP停止”已经确认是`led2`任务栈深度参数错误导致的FreeRTOS栈溢出，不是Flash扫描慢、TCP ACK协议错误或SPI0冲突直接造成。

下一步先修正所有任务创建时的栈深度参数，再单独验证TCP/CAN等待空转导致的任务饥饿问题。

### 栈监控任务

根据这次栈溢出问题，在`FreeRTOSConfig.h`中开启：

```c
#define INCLUDE_uxTaskGetStackHighWaterMark 1
```

随后在`rtos_task.c`中增加优先级为1的`stack_monitor_task`。它不会在中断、Flash、TransmitTask或栈溢出Hook中打印，而是每5秒查询一次各任务的历史最小剩余栈：

```text
stack remain(words): led1=... led2=... collect=... storage=... transmit=... monitor=...
```

数值单位是`StackType_t`，GD32F103中一个字为4字节。例如`transmit=180`表示TransmitTask历史最危险的时候仍至少剩180个字，也就是720字节。

同时把LED任务传给`xTaskCreateStatic()`的栈深度从错误的`32U`改为`256U`，使它与`led1_task_stack[256]`、`led2_task_stack[256]`实际一致。

栈监控任务自身需要调用包含六个数值的`printf`，因此单独分配512个字，不能只因为其他任务使用256个字就机械地使用同样大小。

现在我们通过打印看一下每个任务占用多少
```C
/* 每5秒打印各任务历史最小剩余栈；数值单位为StackType_t，不是字节。 */
static void stack_monitor_task(void *argument)
{
    TickType_t last_wake_time;
    (void)argument;

    vTaskDelay(pdMS_TO_TICKS(5000U));
    last_wake_time = xTaskGetTickCount();
    while(1)
    {
        printf("stack remain(words): led1=%lu led2=%lu collect=%lu storage=%lu transmit=%lu monitor=%lu\r\n",
               (unsigned long)uxTaskGetStackHighWaterMark(led1_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(led2_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(collect_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(storage_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(transmit_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(5000U));
    }
}
```
输出为
```bash
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
stack remain(words): led1=231 led2=230 collect=204 storage=138 transmit=130 monitor=406
```
这里的数值为remain 越小，代表这个任务历史上用过的栈越多。，传输和storagee特别消耗，led翻转倒不会特别占用
//接下来我们来优化storage存储策略
最终目标为优化启动时间+按照空闲16字节进行存储