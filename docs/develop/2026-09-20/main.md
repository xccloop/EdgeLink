2026-09-20

今天主要是在原来的固定 16 字节 Flash 日志上加入循环写入。开始时我以为“循环”只是把写地址到末尾时改回 `0x000000`，后来发现真正麻烦的是：Flash 里会同时存在旧地址上的旧记录、新地址上的新记录、中间的空槽位，以及掉电留下的半写槽位。不能再把“遇到第一个 `0xFF`”理解成日志结束。

这次的日志区仍然是一条记录 16 字节：

```text
[0..3]   sample_uptime_ms
[4..7]   sequence
[8..9]   temperature
[10]     temperature_scale
[11..14] CRC32
[15]     delivery_status，0x00 = confirmed，其他值 = pending
```

Hub 仍然用 `(nodeId, sequence)` 查重，所以 Flash 地址可以回环，`sequence` 不可以跟着回环。地址只是“这条记录现在在 Flash 的哪个槽位”，sequence 才是这条业务消息的长期身份。

## 循环扫描不能在第一个空槽位停止

原来的顺序追加日志中，从 `0x000000` 往后写，第一次遇到全 `0xFF` 的槽位，后面必然也没有有效记录。循环写入后这个前提不成立。

例如日志绕回后，Flash 的物理布局可能是：

```text
低地址：sequence 1549、1550、1551 ...... 新记录
中间 ：一段全 FF 空槽
高地址：sequence 1207、1208、1209 ...... 旧记录
```

如果扫描在中间的全 FF 停止，高地址的旧 pending 就永远找不到；上电恢复会误以为它们不存在。因此现在启动扫描必须读完整个日志区：

```text
全 FF       -> 只表示这个槽位空，继续扫描
CRC 正确    -> 参与最大 sequence、最小 pending 的统计
CRC 错误    -> 不当作有效记录，也不能用它恢复 sequence
```

这里最大的理解变化是：循环日志的“时间顺序”不再等于“从低地址到高地址”。启动时要用 CRC 正确的最大 sequence 找到新记录的末尾，用最小 pending sequence 找到需要恢复的最旧消息。

## 写指针不能从第一个 FF 推出来

写指针应该从最大有效 sequence 的物理地址继续向后推，而不是找第一个空槽。原因还是回环：低地址的空槽可能只是上一次写入还没有绕到那里，并不代表新数据应写在那里。

恢复规则目前是：

```text
没有有效记录：next_sequence = 0，下一写地址 = 日志起点
有有效记录  ：next_sequence = max_sequence + 1，下一写地址 = 最大记录的下一个槽位
max_sequence = 0xFFFFFFFF：不再分配新 sequence，防止 sequence 回到 0 后破坏 Hub 去重
```

这里我还碰到了掉电半写问题。假设 sequence 5 完整写入前掉电，槽位里的 CRC 不正确；重启扫描时最大有效 sequence 仍然是 4。下一条业务消息仍应该是 sequence 5，但不能把这块半写槽位再次当作空 Flash 编程，因为 NOR Flash 只能从 1 写成 0。

所以当前处理是：最大有效记录后，如果仍在同一个扇区，就跳过后面所有不是全 FF 的槽位，直到找到真正空槽或扇区边界，再写新的 sequence 5。这样 sequence 不重复分配，损坏槽位也不会被错误复用。

## 进入新扇区时才擦除

GD25Q32 的最小擦除单位是 4 KiB，一个扇区有：

```text
4096 / 16 = 256 条记录
```

循环日志不能每写一条就擦除一次；正确时机是“准备写一个扇区的第一个槽位”时才擦除该扇区，然后连续写完该扇区的 256 个槽位。

```c
if((storage_next_address % STORAGE_SECTOR_SIZE) == 0U)
{
    gd25_clear(storage_next_address);
}
```

这也明确了本期循环日志的业务边界：日志区彻底写满并再次回到某个扇区时，会擦掉这个扇区里的最旧 256 条记录，其中即使还有 pending 也会丢失。这不是“可靠送达”的保证，而是“网络长期不可用时仍保持最新采集”的有界循环缓存策略。

## 整片擦除卡住时，不能先认定是 Flash 太慢

为了测试，我一度在 `main()` 中循环擦除整个 4 MiB Flash，等了很久 LED 仍然没有点亮，串口也只停在：

```text
CH340 init success
LED init success

KEY init finsh
KEY init success
```

最开始我把它当成“整片 Flash 擦除很慢”。后来继续定位才发现，程序根本没有走到 Flash 擦除：它卡在 `init_all()` 的 BMP280 初始化之前。

根因是 `spi0_bus_init()` 在 FreeRTOS 调度器启动前创建互斥锁。FreeRTOS 的临界区把 `BASEPRI` 留在 `0x50`，而 SysTick 优先级是 `0xF0`，于是 SysTick 被屏蔽，`delay_ms()` 的毫秒计数不再增加。SWD 看到的关键事实是：

```text
board_systick_ms = 0
SysTick CSR = 0x00010007（硬件已经使能）
BASEPRI = 0x50
PC 停在 delay_ms(5U)
```

修复后，SPI0 互斥锁只在调度器已经运行时首次需要锁时创建；调度器启动前的初始化是串行的，不需要互斥锁。这个问题说明串口只打印到哪里，比“我觉得 Flash 擦除慢”更有价值：先确认程序到达了哪一层，再判断外设命令是否慢。

## TCP 断开、CAN 未开启时看到的 sequence

后面测试时，Hub 曾出现下面的输出：

```text
TCP telemetry received: node=1 sequence=1200 temperature_raw=2745 scale=-2
...
TCP telemetry received: node=1 sequence=1206 temperature_raw=2738 scale=-2
CAN rx: id=0x281 node=1 seq=1207 temp_raw=0x0AB1 temp=2737 scale=-2 t=1789916996450498
CAN rx: id=0x281 node=1 seq=1208 temp_raw=0x0AB1 temp=2737 scale=-2 t=1789916996464607
CAN rx: id=0x281 node=1 seq=1209 temp_raw=0x0AB2 temp=2738 scale=-2 t=1789916996481454
CAN rx: id=0x281 node=1 seq=1549 temp_raw=0x0A93 temp=2707 scale=-2 t=1789916997042140
```

我一开始怀疑 sequence 或时间戳转换出错。查询 Hub SQLite 后，`receivedAtUs` 与 CAN 控制台的 `t` 完全一致：

```text
1207  1789916996450498
1208  1789916996464607
1209  1789916996481454
1549  1789916997042140
```

`t` 不是节点采样时间，也不是 Flash 的 `sample_uptime_ms`；它是 Hub 收到帧时调用 `system_clock::now()` 得到的 Unix 微秒时间。`1789916996450498` 转为树莓派本地时间是：

```text
2026-09-20 16:09:56.450498 +0100
```

因此 1207、1208、1209 在十几毫秒内到达，并不是节点在十几毫秒内采集了三次，而是 Hub 在十几毫秒内收到三条已经滞留的 CAN 帧。

这里还确认了一件容易误解的事情：`can0_data_send()` 返回一个发送邮箱编号，只代表帧已经交给 CAN 硬件等待发送，不代表树莓派收到。CAN 未开启时，前 3 帧可以分别占住 3 个硬件发送邮箱；之后邮箱没有空位，新的 CAN 投递直接失败。开启 CAN 后，1207、1208、1209 才会一起快速发到 Hub。1210 到 1548 没有在运行中补发，但应该仍保留为 Flash pending。

## 当前循环日志已经做到和还没有做到的部分

本次循环日志相关代码已经提交：

```text
4a7f260 feat(storage): 优化开机扫描并新增回环日志处理
```

- [x] 全日志区扫描，不再在第一个全 FF 槽位停止。
- [x] 用最大有效 sequence 恢复 `next_sequence` 和物理写指针。
- [x] 写地址回到日志区起点后继续写入，sequence 不回环。
- [x] 进入新 4 KiB 扇区的第一个槽位前擦除该扇区。
- [x] 跳过同扇区中的半写损坏槽位，避免再次编程。
- [x] Hub SQLite 保留 `(nodeId, sequence)` 唯一约束。
- [ ] 没有完成真实断电时“写 16 字节中途掉电”的硬件测试。
- [ ] 没有完成“循环覆盖到含 pending 的旧扇区”后的硬件测试。
- [ ] 当前上电只会把最旧的一条 pending 投递给 TransmitTask，随后立即放行 CollectTask；还没有做到按顺序发送所有 pending 后再进入正常采集。
- [ ] TCP 连接中断后的自动重连，以及运行过程中补发历史 pending，暂时不在本次范围内。

## 下一步

下一步只处理启动恢复：StorageTask 上电扫描得到最旧 pending 后，不立即把 `next_sequence` 发给 CollectTask；而是等待这条记录成功 ACK、确认 Flash，再寻找下一条 pending。所有 pending 成功确认后才放行采集。如果同一条连续三次 TCP + CAN 都没有拿到合法 ACK，则暂时放行采集，pending 保留到下一次上电继续恢复。
