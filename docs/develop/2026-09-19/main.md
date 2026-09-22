2026-09-19

今天把transmit任务的ACK接收补上了。这个事情一开始看起来只是Hub返回一个ACK，node收到后把Flash状态位写成`0x00`。但是实际写的时候才发现，node之前只有TCP发送，没有真正接收TCP业务数据的地方。

ESP12S不是直接把TCP数据交给GD32的TCP协议栈，它是通过USART1把AT回应和网络数据混在一起发回来。所以不能简单地在`tcp_frame_transmit()`后面等一个字符串，也不能把`SEND OK`当成Hub已经落库。`SEND OK`只是ESP12S确认自己把字节交给了它的网络发送流程，Hub是否收到、是否写入SQLite、是否回ACK是后面的事情。

这次先只把接收通路写通：

```text
Hub SQLite插入/重复命中成功
        ↓
Hub通过TCP返回16字节V5 ACK
        ↓
ESP12S USART1收到 +IPD,16:<二进制ACK>
        ↓
USART1_IRQHandler复制16字节到FreeRTOS队列
        ↓
TransmitTask取出、校验CRC/目标node/sequence
        ↓
向StorageTask发送 { sequence, flash_address }
        ↓
StorageTask复读Flash槽位后，只写状态字节为0x00
```

## ESP12S到底是怎样把接收数据告诉单片机的

本项目ESP12S使用单连接模式，也就是`CIPMUX=0`。ESP-AT收到TCP数据后，不会主动把数据当作普通AT回应交给我们，而是从USART1输出：

```text
+IPD,<length>:<raw payload>
```

例如Hub发送一条固定16字节的TCP ACK，串口线上看到的结构可以理解成：

```text
+IPD,16: EH 05 00 node_id sequence(4) 00 00 ack_status CRC32(4)
```

注意`+IPD,16:`前半部分是ASCII文本，冒号后面是二进制帧。二进制帧中完全可能出现`\r`、`\n`、`>`，所以绝对不能继续按“读到换行就是一条回应”的旧逻辑解析。

之前USART1中断只识别下面这些AT文本事件：

```text
OK
ERROR
FAIL
SEND OK
CONNECT
CLOSED
WIFI CONNECTED
WIFI GOT IP
ready
busy...
```

它们仍然保留在原来的小环形队列中，供`tcp_init()`、`tcp_data_send()`等AT状态机取用。`+IPD`则单独走二进制接收状态，不和这些文本事件混在一起。

## 接收中断中的状态

本次在`usart.c`里加入了4个状态变量：

```c
static uint16_t esp12s_ipd_remaining;
static uint8_t esp12s_ipd_store_ack;
static uint8_t esp12s_ipd_ack_index;
static tcp_ack_frame_t esp12s_ipd_ack_frame;
```

它们不是一个完整TCP接收缓存，只是为了当前固定16字节ACK设计的最小状态机。

- `esp12s_ipd_remaining`：当前`+IPD`负载还有多少字节没有从USART1读完。
- `esp12s_ipd_store_ack`：这次负载长度是否正好为16。如果不是16，仍然必须把字节消费掉让串口解析重新同步，但不保存。
- `esp12s_ipd_ack_index`：当前已经复制了ACK的第几个字节。
- `esp12s_ipd_ack_frame`：ISR临时存放的16字节原始数据。这里还没有判断它是不是正确ACK。

这样做的一个关键点是：长度不等于16的`+IPD`不会被误认为ACK，也不能停在冒号后不处理。否则后面真实的AT回应会被当作旧负载的一部分，整个USART解析都会乱掉。

## 中断中的实际流程

`USART1_IRQHandler()`每次只拿走一个已经到达的字节。下面是我理解后的流程：

```text
收到一个USART字节
    ↓
现在是否正在接收+IPD负载？
    ├─ 是：按remaining消费一个字节
    │       ├─ 这次长度为16：复制到ack_frame[index]
    │       └─ remaining变成0：将完整原始帧投递给ACK队列
    │
    └─ 否：继续原有AT文本解析
            ├─ '>' -> 发送PROMPT事件
            ├─ '\r' -> 忽略
            ├─ '\n' -> 将已经收完的文本行翻译成AT事件
            ├─ ':'且当前文本形如+IPD,<数字>
            │       -> 进入+IPD负载接收状态
            └─ 普通字符 -> 暂存到AT文本行缓冲区
```

真正进入负载状态的代码是：

```c
if((receive_data == ':') && (esp12s_ipd_length_get(&ipd_length) != 0U))
{
    esp12s_ipd_remaining = ipd_length;
    esp12s_ipd_store_ack = (ipd_length == TCP_FRAME_LENGTH) ? 1U : 0U;
    esp12s_ipd_ack_index = 0U;
    esp12s_response_line_length = 0U;
    esp12s_response_line_overflow = 0U;
    return;
}
```

`esp12s_ipd_length_get()`只接受严格的`+IPD,`加十进制长度，顺便防止16位长度计算溢出。它不会因为一条普通AT文本里碰巧有冒号就进入二进制接收状态。

在接收负载阶段，代码不再解释任何字符：

```c
if(esp12s_ipd_remaining != 0U)
{
    if(esp12s_ipd_store_ack != 0U)
    {
        esp12s_ipd_ack_frame.data[esp12s_ipd_ack_index] = receive_data;
        esp12s_ipd_ack_index++;
    }

    esp12s_ipd_remaining--;
    if(esp12s_ipd_remaining == 0U)
    {
        if(esp12s_ipd_store_ack != 0U)
        {
            (void)rtos_tcp_ack_frame_send_from_isr(
                &esp12s_ipd_ack_frame,
                &higher_priority_task_woken);
            portYIELD_FROM_ISR(higher_priority_task_woken);
        }
    }
    return;
}
```

这里的`return`很重要。假设二进制ACK的某个字节正好是`'>'`，如果没有提前返回，它会被误当成`CIPSEND`的发送准备提示；假设其中有`'\n'`，它又会被错误送到AT文本行解析。

## 为什么中断只复制，不在里面调用tcp_ack_decode

一开始很容易把`tcp_ack_decode()`直接放进中断：收到16字节就验证CRC、判断sequence，然后写Flash。这样写表面上链路很短，但职责会彻底混乱。

中断现在只做三件事：读取硬件数据寄存器、维护非常小的接收状态、用`xQueueSendFromISR`对应的封装投递完整原始帧。它不等待Hub、不打印、不解析业务语义、不发送AT命令，更不会碰GD25Q32。

原因有几个：

1. CRC、帧字段和sequence匹配属于协议/业务判断，应该在任务上下文完成。
2. Flash的读写权仍在StorageTask。ISR或TransmitTask直接写状态会绕过“先复读CRC和sequence”的保护。
3. `TransmitTask`已经知道自己正在等待哪个`sequence`，它最适合丢弃迟到ACK、其它节点ACK或错误ACK。
4. ISR越短，越不容易影响SysTick、CAN接收和其他FreeRTOS调度。

所以目前队列里存放的是原始的：

```c
typedef struct
{
    uint8_t data[TCP_FRAME_LENGTH];
} tcp_ack_frame_t;
```

它只是“ESP12S刚刚收到一个长度为16的TCP负载”的事实，不代表确认已经成立。

## TransmitTask怎样等待这条ACK

发送一个`transmit_work_item_t`前，TransmitTask先清空上一次残留的TCP ACK队列，再调用`tcp_frame_transmit()`发送遥测帧。只有ESP返回发送成功后，才开始等待不超过1秒：

```c
if(tcp_frame_transmit(&transmit_work.message) == TCP_TRANSMIT_SUCCESS)
{
    if(transmit_tcp_ack_wait(tcp_ack_queue,
                             transmit_work.message.sequence) != 0U)
    {
        transmit_storage_confirm_send(confirm_queue, &transmit_work);
        continue;
    }
}
```

`transmit_tcp_ack_wait()`内部不是空转while。它把剩余等待时间交给`xQueueReceive()`，所以没有ACK时TransmitTask会阻塞让出CPU；有其他无效帧时取出来判断后继续等待直到1秒结束。

TCP ACK要同时满足：

```text
固定16字节
EH + V5
source = 0
target = 当前board_id
温度保留字段为00 00
ack_status = 0
CRC32正确
ACK sequence = 当前发送的message.sequence
```

其中最后一个sequence比较很关键。比如TCP的ACK来得晚：当前工作项已经因为TCP超时转去CAN了，那么它不可以确认另一条新的数据。每一次开始发送前清空旧队列，之后又只接受与当前`expected_sequence`一致的ACK，两个条件一起保证不会把历史回应用错地方。

TCP失败或者一秒内没有得到合法ACK，当前实现才转CAN。CAN ACK也会经过相同的sequence比较。两种传输都失败时，TransmitTask不会发确认事件，StorageTask自然不会把状态写成`0x00`，记录继续保持pending。

## 最后的二次写入为什么还要StorageTask复读一次

TransmitTask收到正确ACK后送出的不是“请把某一个字节写成0”的裸命令，而是：

```c
typedef struct
{
    uint32_t sequence;
    uint32_t flash_address;
} storage_confirm_event_t;
```

StorageTask收到事件后会：

1. 先读回这个`flash_address`对应的16字节槽位。
2. 校验记录原本的CRC。
3. 读取槽位内的sequence，并和事件中的sequence比较。
4. 两个条件都正确，才单独把状态字节写成`0x00`。
5. 再读回状态字节；只有精确读到`0x00`，本次`storage_confirm()`才返回成功。

因此伪造的`flash_address + sequence`组合、Flash CRC已经坏掉的槽位，或者状态写入失败，都不会被当成confirmed。状态写入中断电后，即使读到`0xF0`、`0xC0`这样的中间值，启动扫描仍按pending处理并重发，这就是这里选择at-least-once而不是“尽量刚好一次”的原因。

## 这次已经验证和还没有验证的事情

本次代码提交为：

```text
360ae25 (feat)transmit任务增加ACK应答
```

构建已经通过：

```text
text    data     bss     dec     hex
30140   116      11424   41680   a2d0
```

- [x] 源码：USART1将`+IPD,16:`的负载与AT文本回应分开。
- [x] 源码：ISR只投递原始帧，协议校验和Flash确认留在任务上下文。
- [x] 源码：TCP超时后才进入CAN ACK等待；无ACK不确认Flash。
- [x] 构建：Node全量编译通过。
- [ ] 烧录：这次修改后还没有重新烧录验证。
- [ ] 串口：还没有抓到真实的`+IPD,16:`原始输出。
- [ ] TCP端到端：还没有证明Hub ACK到达ESP12S、被正确解码并触发状态确认。
- [ ] CAN回退：还没有证明TCP超时后CAN ACK能确认同一条Flash记录。
- [ ] 掉电：还没有做“状态写到一半断电，重启后仍重发”的物理测试。

## 下一步

开始实现HMI+Battery监测
