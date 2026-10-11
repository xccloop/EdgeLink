# 2026-10-10 开发笔记

这几天一直开发做测试，很久没有开始连通性调整了，目前项目已经实现了基本功能，足够开始实际演示了

## 1. 测试单节点正常启动，TCP/CAN 均可用
本次测试中会把tcp/can都单独使用，进行遥测正常
预测结果：无论，TCP还是CAN，节点都会正常上报数据
遇到个情况，运行脚本显示

```text
(base) PS C:\Users\memory\Desktop\EdgeLink> ./edgenode/tools/compiled.ps1
./edgenode/tools/compiled.ps1 : 无法加载文件 C:\Users\memory\Desktop\EdgeLink\edgenode\tools\compiled.ps1。未对文件 C:\Users\memory\Desktop\EdgeLink\edgenode\tools\compiled.ps1 进
行数字签名。无法在当前系统上运行该脚本。有关运行脚本和设置执行策略的详细信息，请参阅 https:/go.microsoft.com/fwlink/?LinkID=135170 中的 about_Execution_Policies。
所在位置 行:1 字符: 1
+ ./edgenode/tools/compiled.ps1
+ ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    + CategoryInfo          : SecurityError: (:) []，PSSecurityException
    + FullyQualifiedErrorId : UnauthorizedAccess
```

这种时候要给终端开权限
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
随后再运行就好了
说回测试，TCP/CAN运行均无问题，详情视频在（C:\Users\memory\Desktop\EdgeLink\docs\develop\2026-10-10\TEST\test1）

## 2. 测试不同节点的上报
我会使用两块节点，烧录同一固件（除了board_id），测试不同节点上报是否产生冲突
测试成功，测试视频放在（C:\Users\memory\Desktop\EdgeLink\docs\develop\2026-10-10\TEST\test2）

## 3. 测试 TCP 正常 / CAN 不可用，以及 TCP 不正常 / CAN 可用
测试结果在C:\Users\memory\Desktop\EdgeLink\docs\develop\2026-10-10\TEST\test3
一个小bug：当CAN正常，TCP不正常的时候，单节点上报数据can会积压两三条才发出去，导致会有一些记录变成pending，并且由于不进行CAN自动重连导致如果两条链路都损坏
先恢复的CAN反而不工作
，感觉是因为TCP重连浪费了时间导致堵塞

## 4. 测试 HMI 人机交互
测试HOME、LINKS、LOG、STORAGE，操作三个按键，温度、链路结果、日志、pending 数量与实际一致；切页和去抖正常。
测试结果在C:\Users\memory\Desktop\EdgeLink\docs\develop\2026-10-10\TEST\test4
大部分功能显示均正常，但是又一个小bug，pending记录会+1，-1的循环
这是由于我们先记录一个记录为pending，此时+1，然后等发送成功后将pending取消，此时-1，就会导致IPS上会显示+1,-1的循环

## 5. 测试连续采集 flash 写入是否正常
测试跨页时候flash是否可以正常擦除
具体结果在C:\Users\memory\Desktop\EdgeLink\docs\develop\2026-10-10\TEST\test5
此时的序列号来到了6w5左右程序依然正常运行，测试成功，说明日志正常写入，并没崩溃

## 6. 测试补发
TCP/CAN 都断开，产生多条 pending，再恢复
测试结果为：CAN中途恢复不补发，用CAN为主链路的时候会积压pending即使在正常发送（不清楚是IPS的bug还是链路真的有bug）
TCP重连自动补发，并且重启后，TCP/CAN的一路会自动补发pending
结果在C:\Users\memory\Desktop\EdgeLink\docs\develop\2026-10-10\TEST\test6

## 7. 测试运行中 hub 断线
运行中停止 Hub，再启动
测试成功，中途断线后，CAN/TCP均会重连并且开始运行
TCP重连会把积压的pending重发，因此他的序号在hub断开前后是连续的，CAN则是只会发送当前的不进行补发
详细视频在C:\Users\memory\Desktop\EdgeLink\docs\develop\2026-10-10\TEST\test7

## 阶段性总结：CAN 链路的问题
1.can重连不补发
2.can由于TCP重连导致堵塞
3.由于堵塞导致pending记录偶尔会上涨，感觉是因为队列塞不下导致的

can补发倒是简单，我们只需要把CAN重连的时候增加补发逻辑就好了

```c
if(transmit_success == 0U)
            {
                transmit_can_receive_queue_clear(can_receive_queue);
                if(can_frame_transmit(board_id,
                                      &transmit_work.message) == CAN_TRANSMIT_SUCCESS)
                {
                    link = "CAN";
                    if(transmit_can_ack_wait(can_receive_queue,
                                             transmit_work.message.sequence) != 0U)
                    {
                        if(can_state == HMI_LINK_OFFLINE)
                        {
                            link_restored = 1U;
                        }
                        can_state = HMI_LINK_ONLINE;
                        transmit_success = 1U;
                    }
                }
            }
```

link_restored就是补发标志位，他背后的逻辑都是同一套补发逻辑

## 发现新问题：TCP 链路长期运行会掉线

> 哪怕我 node 重启之后，hub 还是无法接收到，只有 hub 重启才会接收到。

**关键发现**：当 TCP 不上报的时候，尝试去 ping node2 的地址，几乎同时 ping 的消息与上报消息同时抵达，难道说是有休眠机制？

## 现场取证（故障出现时的观察）

### 1. Hub 上挂着 6 条 node2 的僵尸连接，最老的已经死了 2.3 小时

```text
ESTAB  192.168.1.112:8888 192.168.1.115:41871   fd=8    死了 8363 s ≈ 2.3 h
ESTAB  192.168.1.112:8888 192.168.1.115:41221   fd=9    死了 6550 s ≈ 109 min
ESTAB  192.168.1.112:8888 192.168.1.115:35511   fd=10   死了 4616 s ≈ 77 min
ESTAB  192.168.1.112:8888 192.168.1.115:13132   fd=11   死了 3250 s ≈ 54 min
ESTAB  192.168.1.112:8888 192.168.1.115:39718   fd=12   死了 1913 s ≈ 32 min
ESTAB  192.168.1.112:8888 192.168.1.115:33074   fd=13   死了  105 s
```

Hub 09:54 启动，2 小时 39 分里 node2 重连了 6 次，每一条旧连接都留在 clients[] 里从没被关掉。你之前那批 6 个槽位已经占掉，再攒 14 次就是 Too many clients。（顺带：Hub 不是 systemd 跑的，是 ./build/Edgehub 挂在 /dev/pts/0 上，所以 journalctl 什么都看不到。）

### 2. 每条连接断的瞬间，形状完全一样

```text
:33074  bytes_sent:27776  bytes_acked:27760  bytes_received:27776  cwnd:2 ssthresh:7
:41871  bytes_sent:84160  bytes_acked:84144  bytes_received:84144  cwnd:2 ssthresh:7
bytes_sent - bytes_acked = 16 —— 差的就是 Hub 最后发出去的那个 16 字节 ACK，ESP 从没 TCP 确认过它。所有 6 条都是这个死法。

```

### 3. 照着同样的做法 ping 一次，现象一模一样

```text
6 packets transmitted, 2 received, 66.6667% packet loss
icmp_seq=5 time=1091 ms      ← 你的那次是 icmp_seq=5 time=996 ms
两次都是"前 4 个全丢，第 5 个迟到约 1 秒回来"。 这不是巧合，是定时器。

```

### 4. 恢复的方式很重要：是新建连接，不是救活旧连接
ping 之后立刻快照，多出一条 192.168.1.115:38058，原来 6 条一条没动，还是僵尸。同时 ip neigh 从 STALE 变成 REACHABLE，但 MAC 地址没变（10:52:1c:06:9d:df，前后一致）。

### 5. 恢复之后 node2 完全正常

```text
12:35:50 node2: 22640 → 12:35:53: 22643 → 12:35:56: 22646 → 12:35:59: 22649
10 packets transmitted, 10 received, 0% packet loss, rtt avg 11.5 ms
```

所以 radio 本身没问题——66% 丢包只出现在它"聋掉"的那几秒。

所以这不是简单的掉线重连，因为明显开不同的fd客户端，这也引出一个问题，这个僵尸客户端是不该存在的，掉线以后不应该还占用槽位，不然就会导致槽位泄露

又遇到一个问题，不乏后会卡死堵塞一段时间，感觉像是由于补发完成后无条件扫描pending记录导致的

---

## node2 断流排查全过程（命令 → 输出 → 怎么判断）

下面按"我每一步想知道什么"来记。重点不是结论，是**为什么看到这个数字就能下这个判断**。

### 第 0 步：先确认"日志"到底在哪 —— 不然后面全是瞎猜

```bash
ps -o pid,ppid,lstart,etime,cmd -p 4697
ls -l /proc/4697/fd/1 /proc/4697/fd/2
systemctl list-units --all | grep -iE "hub|edge"
journalctl -u edgehub -n 40 --no-pager
```

输出：

```
PID   PPID   STARTED              ELAPSED   CMD
4697  1109   Sat Oct 10 09:54:23  02:39:34  ./build/Edgehub
fd/1 -> /dev/pts/0        fd/2 -> /dev/pts/0
(没有匹配的 unit)
-- No entries --
```

判断依据：

- `PPID = 1109`，而 1109 是 `-bash` → 它是**手动跑起来的**，不是 systemd 拉起的。
- `fd/1 -> /dev/pts/0` → `printf` 的输出全进了那个终端，**一个字节都没落盘**。
- `journalctl` 回 "No entries" 很容易误判成"服务根本没起"。**它只是不在 systemd 手里**，跟起没起没关系。

结论：**没有历史日志可查**。所以后面得换一个东西当"日志"——拿 SQLite 的写入进度当心跳。这一步如果跳过，后面看到的每个现象都没有时间戳可以对齐。

### 第 1 步：node2 还在不在上报？（用数据库当心跳）

```bash
python3 -c "
import sqlite3
c=sqlite3.connect('file:/home/qxc/Desktop/EdgeLink/edgehub/data/edgehub.db?mode=ro',uri=True)
for r in c.execute('select nodeId,count(*),max(sequence),max(receivedAtUs) from messages group by nodeId'):
    print(r)"
```

**必须连着跑 3 次**，每次隔 3~4 秒。输出：

```
12:34:36  node=1 rows=45343 maxseq=46335
12:34:44  node=1 rows=45343 maxseq=46335      ← 17 天没动，早就下线了
12:34:36  node=2 rows=22461 maxseq=22461
12:34:40  node=2 rows=22461 maxseq=22461
12:34:44  node=2 rows=22461 maxseq=22461      ← 三次完全一样 = 冻结
12:34:36  node=3 rows=76967 maxseq=77295
12:34:40  node=3 rows=76971 maxseq=77300      ← 一直在涨
12:34:44  node=3 rows=76975 maxseq=77304
```

判断依据：

- **单次查询什么也看不出来**，必须连续几次看它涨不涨。这是整个排查里最容易犯的错。
- node2 三次一模一样 → 冻住了。node3 一直在涨 → **Hub 进程是活的，只是 node2 的数据进不来**。这一步顺手否掉了"整个 Hub 卡死"这个方向。
- `max(receivedAtUs)` 是微秒时间戳，可以换算成"最后一次收到是多久以前"：`(now - last) / 1e6 / 60` = 分钟。**这个数后面要反复用。**
- `mode=ro` + `uri=True` 是只读打开，不会干扰正在运行的 Hub（SQLite 有多进程读锁）。

### 第 2 步：Hub 上到底挂着几条连接

```bash
sudo ss -tanp | grep ':8888'
```

输出：

```
LISTEN 0 10 0.0.0.0:8888 0.0.0.0:*  users:(("Edgehub",pid=4697,fd=4))
ESTAB  0  0 192.168.1.112:8888 192.168.1.115:39718 users:(("Edgehub",pid=4697,fd=12))
ESTAB  0  0 192.168.1.112:8888 192.168.1.115:33074 users:(("Edgehub",pid=4697,fd=13))
ESTAB  0  0 192.168.1.112:8888 192.168.1.115:41871 users:(("Edgehub",pid=4697,fd=8))
ESTAB  0  0 192.168.1.112:8888 192.168.1.115:13132 users:(("Edgehub",pid=4697,fd=11))
ESTAB  0  0 192.168.1.112:8888 192.168.1.115:35511 users:(("Edgehub",pid=4697,fd=10))
ESTAB  0  0 192.168.1.112:8888 192.168.1.115:41221 users:(("Edgehub",pid=4697,fd=9))
```

判断依据：

- 固件里设的是 `AT+CIPMUX=0`（**单连接模式**），所以同一个 node 在同一时刻**只该有一条** TCP 连接。
- 出现 6 条 → 前 5 条死了，但**没有被关掉**。
- `fd=8..13` **连号** → 说明它们是一条条累积出来的，不是启动时就有的一堆。这跟"运行中反复重连"对得上。
- `ss -p` 需要 root 才能看到进程名，所以要 `sudo`。

### 第 3 步：每条连接"死"的细节 —— 这一步是转折点

```bash
sudo ss -tanio | grep -A1 ':8888' | grep -v LISTEN
```

`ss -i` 会多打一行内部状态。要看的字段：

| 字段 | 意思 |
| --- | --- |
| `lastsnd` / `lastrcv` / `lastack` | 距离上次**发送 / 接收 / 收到 ACK** 多少**毫秒** |
| `bytes_sent` / `bytes_acked` / `bytes_received` | 累计发出 / 被确认 / 收到 的字节数 |
| `cwnd` / `ssthresh` | 拥塞窗口 / 慢启动阈值 |
| `retrans` | 重传计数 |

输出（挑两条）：

```
:41871  bytes_sent:84160 bytes_retrans:16 bytes_acked:84144 bytes_received:84144
        cwnd:2 ssthresh:7 lastsnd:8363428 lastrcv:8363440
:33074  bytes_sent:27776 bytes_acked:27760 bytes_received:27776
        cwnd:2 ssthresh:7 lastsnd:104844  lastrcv:104864
```

判断依据：

1. `lastsnd ≈ lastrcv`（只差几毫秒）→ Hub 最后一次**发**和最后一次**收**是同一瞬间，之后彻底静默。如果是"对端在发但 Hub 没读"，`lastrcv` 会是最近的；反过来如果是"Hub 在发但没人回"，`lastack` 会卡住。**三个值互相对比就能分清是哪个方向断的。**
2. `8363428 ms ÷ 1000 ÷ 60 ≈ 139 分钟` → 这条已经死了 2.3 小时。**`lastsnd` 是这里唯一能给出"死了多久"的字段。**
3. `bytes_sent - bytes_acked = 16` → Hub 有 16 字节发出去了、对方**从没确认**。一个 ACK 帧正好是 16 字节（`TCP_FRAME_LENGTH`）→ **最后一个 ACK 卡在半路**。六条全是这个死法。
4. `cwnd:2, ssthresh:7` → 拥塞窗口被砍到 2，说明**中间真发生过丢包**，TCP 进了拥塞避免。正常的连接应该是 `cwnd:10`。

### 第 4 步：ping 一次，把现象复现出来

```bash
ping -c 6 -i 1 192.168.1.115
```

输出：

```
6 packets transmitted, 2 received, 66.6667% packet loss
64 bytes from 192.168.1.115: icmp_seq=5 ttl=128 time=1091 ms
64 bytes from 192.168.1.115: icmp_seq=6 ttl=128 time=68.5 ms
```

判断依据：

- **形状和你上次完全一样**：前 4 个全丢，第 5 个迟到约 1 秒。两次独立复现同样的形状 → 这不是偶发，是有固定触发条件的。
- "第 5 个"这个**位置**才是重点：ping 间隔 1 秒，第 5 个在 t≈4s 发出、t≈5.09s 收到回包。**"5 秒"是 Linux 邻居表一个很标准的定时器**（`net.ipv4.neigh.default.delay_first_probe_time = 5`）。看到固定的第 N 个，就该去找定时器，而不是去找概率。
- 66% 丢包看着像链路烂，但第 6 步会证明这是**假象**。

### 第 5 步：ping 前后对比 socket —— 判断"到底是怎么恢复的"

```bash
sudo ss -tan | grep ':8888' | grep ESTAB | awk '{print $5}'   # ping 之前
#  ... 执行 ping ...
sudo ss -tan | grep ':8888' | grep ESTAB | awk '{print $5}'   # ping 之后
ip neigh show 192.168.1.115
```

输出：

```
ping 前: :39718 :33074 :41871 :13132 :35511 :41221
ping 后: :39718 :38058 :33074 :41871 :13132 :35511 :41221     ← 多出 38058
ip neigh: STALE  →  REACHABLE        MAC 10:52:1c:06:9d:df 前后不变
```

判断依据：

1. 多出一个**新的本地端口 38058** → node2 是**重新建了一条连接**，不是把旧连接救活。旧的 6 条原封不动，正好印证第 2 步的僵尸判断。
2. ARP 表 `STALE → REACHABLE`，但 **MAC 前后完全一样** → 排除了"缓存指向了错误的 MAC"（ARP 污染 / IP 冲突）这条路。**如果 MAC 变了，整个方向会完全不同。**
3. 那 ARP 为什么从 STALE 变 REACHABLE？Linux 在 STALE 状态下会**先用旧 MAC 发一次**（这次丢了），5 秒后自动补发 ARP 请求重新确认。→ **那个迟到 1 秒的 ping，其实是在等 ARP 请求回来。** ping 只是"顺便把 ARP 请求带出来了"。

### 第 6 步：决定性实验 —— 只发 ARP，不发 ICMP

假设：如果真是 ARP 请求叫醒了它，那单独发 ARP 也该有效。

`arping` 没装，所以用 Python 原始套接字手工拼一个 ARP 请求（一个 42 字节的以太网帧，**不含任何 IP 包**）：

```bash
sudo python3 - <<'PY'
import socket, struct
iface='wlan1'
mac=open('/sys/class/net/%s/address'%iface).read().strip()
src=bytes.fromhex(mac.replace(':',''))
spa=socket.inet_aton('192.168.1.112')
tpa=socket.inet_aton('192.168.1.115')
eth=b'\xff'*6+src+struct.pack('!H',0x0806)                       # 广播 + 类型=ARP
arp=struct.pack('!HHBBH',1,0x0800,6,4,1)+src+spa+b'\x00'*6+tpa   # 操作码 1 = 请求
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW); s.bind((iface,0))
s.send(eth+arp); s.close()
PY
```

输出：

```
12:56:04  baseline                     maxseq=23791   ← 已经冻了 45 秒
12:56:06  发出 1 个 ARP 请求
12:56:07                               maxseq=23791
12:56:10                               maxseq=23799
12:56:13                               maxseq=23843   ← 补发积压 +44 条
12:56:40                               maxseq=23845
12:56:43                               maxseq=23848
ip neigh: STALE → REACHABLE
```

判断依据：

- **全程没发过一个 ICMP，node2 照样恢复了** → 叫醒它的不是 ping，是 ARP 请求。假设成立。
- 恢复分两段：先一口气补发积压（23799 → 23843，44 条），再回到正常节奏（~1 条/秒）。说明 node2 内部的 pending 队列**一直是满的**——它一直在等这条路通，不是它自己停了。
- 这一步也回答了你原来那个疑问"难道说是有休眠机制？"：**不是休眠，是它发不出去。**

### 第 7 步：时间对账

```
12:35:15   上一次恢复（我 ping 的那次）
12:55:20   这次冻住
差 = 20 分 05 秒
```

ESP8266 跑的是 lwIP，里面 `ARP_MAXAGE` 默认 **240**，单位是 `ARP_TMR_INTERVAL`（5 秒）→ **240 × 5 = 1200 秒 = 20 分钟**。

20 分 05 秒 ≈ 20 分钟。**这不是巧合，是那条 ARP 表项的有效期。** 到这一步，"为什么是周期性的"就有答案了。

### 第 8 步：回到代码里找"为什么它自己好不了"

| 位置 | 看到什么 |
| --- | --- |
| `Drivers/BSP/ESP12S/esp12s.h` | `AT+CIPCLOSE`、`AT+RST` 都定义了，**全工程一次都没发过** |
| `User/App/Output/Tcp/tcp.c` `tcp_try_reconnect()` | 整个函数只发一条 `AT+CIPSTART` |
| `Drivers/BSP/INTERRUPT/USART/usart.c` | 行匹配用的是**完整相等**比较，`ALREADY CONNECTED` 不等于 `CONNECT`，匹配不上 |
| `Drivers/BSP/INTERRUPT/USART/usart.h` | `ESP12S_RESPONSE_READY` 解析得出来，但没有任何代码处理它 |
| `User/App/FreeRtos/Tasks/Transmit/transmit_task.c` | `(void)tcp_init(...)`，返回值被丢掉 |
| `edgehub/src/Tcp.cpp` `init()` | 只设了 `SO_REUSEADDR`，**没设 `SO_KEEPALIVE`** |
| `edgehub/src/Gateruntime.cpp` `handleTcpClient()` | 只有 `EPOLLERR / EPOLLRDHUP / EPOLLHUP` 才会关连接，事件循环里也没有空闲巡检 |
| `edgehub/inc/Gateruntime.hpp` | `MAX_CLIENTS` = 20，满了走 `rejectClient()` 直接关掉新连接 |

### 整条因果链（第一版结论，后被推翻，保留作记录）

1. ESP8266 里"Hub 的 IP → Hub 的 MAC"那条 ARP 表项，约 20 分钟后过期。
2. lwIP **不会自己重新解析**，它只是把要发给 Hub 的包**全部丢掉**。
3. 于是 node2 停止上报；它 ping 不回来（**收得到、回不出去**，回包要路由到 Hub）；它的重连 SYN 同样发不出去，**三次握手永远完不成**。
4. Hub 这边那条连接看起来还好好的（这次 `bytes_sent == bytes_acked`，**没有东西要重传**）→ **Hub 没有任何理由去重新 ARP** → 故障永久化。
5. 只有"从对面发来的、询问它自己 IP 的 ARP 请求"能把它治好——因为这会让 ESP 顺手把请求里的发送方（Hub 的 IP/MAC）写回自己的 ARP 表。
6. 你 ping 之所以有用，是因为 Linux 邻居表在 5 秒后会自动补发一个 ARP 请求。**ping 从来只是带出 ARP 请求的工具。**

### 还没证实的部分

**为什么那条 ARP 表项会过期、而且过期后重建不了？** 目前最像的解释是 **ESP 的 ARP 表被局域网里其他设备的 ARP 流量挤满**（lwIP 默认 `ARP_TABLE_SIZE` 只有 10），表满之后既淘汰了 Hub 那项、又新建不了，连 ARP 请求都发不出去。这一条我**没有直接证据**，是目前唯一还悬着的环节。

想验证的话，下次冻结时先发一个**不会让 ESP 学到 Hub MAC** 的包（比如在 ARP 表还是 STALE 时发一个 UDP 包，此时 Linux 不会发 ARP 请求），看能不能唤醒：
- 能唤醒 → 是 ESP 的接收通路卡住，跟 ARP 表无关；
- 唤不醒、只有 ARP 请求才行 → 就是 ARP 表的问题，上面那条成立。

---

## 16:30 复现 + 判别实验：**ARP 结论被推翻，真正的 bug 找到了**

### 一、先把判断依据换掉

用户提醒过 nodeId 编号有点乱、不可靠，所以这次**不看 SQLite 的 maxseq**，改用 TCP 连接自身的计数器判断"节点还在不在发"：

```bash
sudo ss -tanio | grep -A1 <节点IP>
```

- `bytes_received` 涨不涨 → 节点有没有发数据过来
- `bytes_sent` vs `bytes_acked` → Hub 发的 ACK 有没有被 ESP 确认（= ESP 的发送通路正不正常）
- `cwnd` / `retrans` → 有没有发生丢包/拥塞

这一套完全不依赖 nodeId 映射，比查库可靠。

### 二、判别实验：UDP vs 纯 ARP 请求

**目的**：12:56 那次我用"纯 ARP 请求能唤醒 node2"推出"ESP 的 ARP 表过期"。但这个结论跟另一条证据矛盾（那条连接 `bytes_sent == bytes_acked`，说明 ESP 当时还能正常发 ACK）。要把"ARP 请求"和"任意一个入站帧"分开。

**方法**：先把邻居表设成 `permanent`，这样 Linux **永远不会**为这个 IP 发 ARP 请求，然后再发 UDP——如果这样也能唤醒，就说明跟 ARP 无关。

```bash
IP=192.168.1.113
MAC=$(ip neigh show $IP | awk '{for(i=1;i<=NF;i++) if($i=="lladdr") print $(i+1)}')

# 1) 锁死邻居表 → Linux 从此不会发 ARP 请求
sudo ip neigh replace $IP lladdr $MAC nud permanent dev wlan1

# 2) 实验1：纯 IP 包
sudo python3 -c "
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.sendto(b'probe',('$IP',9)); s.close()"

# 3) 实验2：纯 ARP 请求（广播，42 字节以太网帧，不含任何 IP 包）
sudo python3 - <<'PY'
import socket, struct
iface='wlan1'
src=bytes.fromhex(open('/sys/class/net/%s/address'%iface).read().strip().replace(':',''))
spa=socket.inet_aton('192.168.1.112'); tpa=socket.inet_aton('192.168.1.113')
eth=b'\xff'*6+src+struct.pack('!H',0x0806)
arp=struct.pack('!HHBBH',1,0x0800,6,4,1)+src+spa+b'\x00'*6+tpa
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW); s.bind((iface,0))
s.send(eth+arp); s.close()
PY
```

**结果：两个都毫无反应。**

```
基线               bytes_received=24880
实验1  UDP 之后    bytes_received=24880     ← 完全没动
实验2  ARP 之后    bytes_received=24880     ← 完全没动
```

**结论：12:56 那次是巧合。** node2 自己的某个慢操作正好在那一刻结束，我把功劳记给了 ARP。**"ESP 的 ARP 表过期"这个结论作废。**

### 三、同时，连接计数器证明网络侧完全健康

```
bytes_sent:24864  bytes_acked:24864    ← Hub 发的 ACK 全部被 ESP 确认
cwnd:10, retrans 0                      ← 无拥塞、无丢包、无重传
bytes_received:24880 卡住十几分钟不动    ← 节点一个字节都没发
```

**ESP 的 TCP 收发是好的。是节点自己不往外发。** 入站的 UDP 和 ARP 广播都叫不动它——网络层的招数已经用尽了，问题不在网络上。

### 四、决定性的一行串口输出

冻结时串口在刷：

```
ESP response: ERROR
```

这行**只在 `User/App/Output/Tcp/tcp.c:73` 一处打印**，属于 `tcp_wait_response()`。而运行期唯一调用 `tcp_wait_response()` 的地方是 `tcp_try_reconnect()`（`tcp.c:379`），它只发一条 `AT+CIPSTART`。

**所以：节点在反复发 `AT+CIPSTART`，ESP 回 `ERROR`。**

ESP-AT 在 `CIPMUX=0`（单连接模式，`tcp_init()` 里设的）下只允许存在一条连接。只要它内部那条 socket 还在，`AT+CIPSTART` 的回应就是：

```
ALREADY CONNECTED
ERROR
```

### 五、完整因果链（第二版结论，也被推翻，保留作记录）

1. 某个瞬间 ESP 没在 10 秒内回 `>` 或 `SEND OK`（`TCP_AT_TIMEOUT_MS = 10000`），或者回了 ERROR/BUSY/FAIL。
2. `tcp_data_send()` 于是把 `tcp_connected` 置成 `TCP_FAIL`（`tcp.c:427` / `tcp.c:434`）。
3. **`tcp_data_send()` 开头就把自己挡死了**：
   ```c
   if((data == NULL) || (length == 0U) || (tcp_connected == TCP_FAIL))
       return TCP_SEND_FAIL;      // tcp.c:396
   ```
   → 从这一刻起，**再也往 ESP 发不出一个字节**。
4. `transmit_task` 每帧判断 `tcp_connected_get() == TCP_FAIL`，于是每 5 秒调一次 `tcp_try_reconnect()`。
5. `tcp_try_reconnect()` **只发 `AT+CIPSTART`**。而 ESP 上那条连接其实还好好的 → 回 `ALREADY CONNECTED` + `ERROR`。
6. 解析器用**完整相等**比较（`usart.c:103`），`ALREADY CONNECTED` 匹配不上 `CONNECT`；紧接着的 `ERROR` 命中 → 串口打出 `ESP response: ERROR` → 返回 FAIL → `tcp_connected` 保持 0。
7. **死循环。** `AT+CIPCLOSE`（`esp12s.h:56`）和 `AT+RST`（`esp12s.h:13`）定义了但**全工程从没发过**，node 没有任何手段拆掉 ESP 上那条连接。
8. **按 node 复位键也没用**：ESP 不掉电，socket 还在 → 重跑 `tcp_init()` 走到 CIPSTART 照样吃 `ALREADY CONNECTED` + `ERROR`。而 `transmit_task.c:179` 是 `(void)tcp_init(...)`，**返回值被丢掉了**，所以连"初始化失败"都没人知道。
9. **只有重启 Hub 才有效**：Hub 关掉 socket → ESP 收到 FIN/RST → 它自己把连接清掉 → 下一次 CIPSTART 才可能成功。

**这一条链解释了你最初报告的全部现象**，而且今天的两个"负结果"（UDP/ARP 无效、连接计数器全绿）恰恰是它的佐证——因为问题根本不在网络上。

**核心缺陷一句话：`tcp_connected` 一旦变成 0 就再也回不来，而 `tcp_data_send()` 又被它挡住——这是个不可逆的自锁。而解锁需要的 `AT+CIPCLOSE` 代码里定义了却没调用。**

### 六、连带发现：上电扫描要一分钟

排查 node2 上电"卡死"时量出来的：`log_scan_log()`（`log.c:253`，由 `log_init()` 在 `gd25_init()` 之后调用）从 `LOG_BASE` 扫到 `LOG_END` = 3,932,160 字节，每次读 256 B，而 `gd25_read()` 内部是**一个字节一次 `spi0_tansfer_data()`** → **393 万次单字节 SPI 调用**。SPI0 = APB2 72 MHz ÷ `SPI_PSC_16` = 4.5 MHz（`spi0_bus.c:67`），纯线上约 7 秒，加上轮询开销**几十秒到一分多钟**。

所以上电打完 `chip_id` 之后那 90 多秒不是死机，是在扫 Flash。这跟"补发后扫描 pending 导致卡顿"是同一类问题（`log_find_next_pending` 的早退补丁只覆盖了没有 pending 的情况）。

---

## 17:26 单变量实验：扳机是 ARP，根因定案

### 先说为什么又推翻了一次

16:30 那一节我写了"ARP 结论被推翻"。**那次实验本身有两个毛病：**

1. **观察窗口只有 18 秒**，而真正的恢复需要 24 秒。**窗口比现象本身还短，那"没反应"这三个字什么都不能说明。**
2. 顺手把邻居表设成了 `permanent`，等于同时把 Linux 自己会发的 ARP 探测也关掉了。

**教训**：一个没有受控、而且观察窗口短于现象本身的负面结果，没有资格推翻一个受控实验拿到的正面结果。我把"没看到"当成了"没发生"。

### 实验设计（一次只动一个变量）

```bash
# 阶段0：什么都不做，静置 45 秒（排除自愈）
# 阶段1：只发一个 ARP 请求 —— 42 字节以太网帧，不含任何 IP 包
# 阶段2：只发一个 ping —— 单个 ICMP echo
```

### 结果

```text
阶段0：静置 45 秒（什么都不做）
  17:25:59  bytes_received=76544  conns=3
  17:26:14  bytes_received=76544  conns=3     ← 纹丝不动
  17:26:29  bytes_received=76544  conns=3     ← 45 秒完全冻死

阶段1：只发一个 ARP 请求
  >>> ARP 请求已发出
  17:26:59  bytes_received=79072  conns=4     ← 涨了 2528 字节，活了
  17:27:14  bytes_received=79312  conns=4
  17:27:29  bytes_received=79552  conns=4

阶段2：ping —— 它早就好了（5.6 ms 秒回）
```

**结论：扳机就是 ARP 请求。** 45 秒静置毫无反应 → 一个纯 ARP 请求 → 24 秒内恢复（`conns` 3→4，多出一条新连接，之后稳定在约 1 条/秒）。

**ping 是间接手段，不是它在起作用**：Linux 邻居表在 `delay_first_probe_time`（5 秒）后会自动补发一个 ARP 请求 —— 所以永远是"第 5 个 ping"。**用户从第一轮就观察到的"ping 和上报几乎同时到"是对的**，我此前两次否定它是错的。

### 最终根因链

```text
① ESP8266 里"Hub 的 IP → Hub 的 MAC"那条 ARP 表项每 ~20 分钟过期
   （lwIP ARP_MAXAGE = 240 × 5s = 1200s）
        ↓  实测三次：20分05秒 / 20分13秒 / 20分21秒
② 表项没了，ESP 就发不出任何东西给 Hub → 节点停止上报
   （ESP 仍能收、仍能回 ARP reply —— reply 的 MAC 可以直接从请求里抄，不用查表）
        ↓
③ AT+CIPSEND 最终失败 → tcp_connected 置 0
   而 tcp_data_send() 开头就 if(...||tcp_connected==TCP_FAIL) return TCP_SEND_FAIL;
   → 不可逆自锁，此后一个字节都发不出去
        ↓
④ MCU 每 5 秒重发 AT+CIPSTART，永远吃 ALREADY CONNECTED + ERROR
   （CIPMUX=0 只允许一条连接，ESP 那条旧 socket 还在）
        ↓
⑤ 唯一的解锁路径是 ESP 自己丢掉旧 socket（TCP 重传超时）
   耗时不定：实测 50 秒 / 161 秒 / 9 分钟
   之后 CIPSTART 成功 → 新连接 → 从节点自己的 flash 日志补发积压
   【旧连接永远不会复活】
        ↓
⑥ 每恢复一次，Hub 上多留一条僵尸连接
   没有 SO_KEEPALIVE、事件循环里没有空闲巡检、accept 时也不按 node 去重
   → clients[] 槽位只增不减
   MAX_CLIENTS = 20 → 满了走 rejectClient() 关掉一切新连接
   → "只有重启 Hub 才行"
```

### 还没解释的两处（先记着）

1. **正常运行时 Hub 每秒都在回 ACK**，按理说早该触发邻居状态机的 `STALE → DELAY → PROBE`，也就该定期发出 ARP 请求 —— 可这条 ARP 过期照样每 20 分钟来一次。**为什么正常流量没能阻止它，我解释不了。** 所以"Hub 定时发 ARP"只能算保险，不能当主修法。
2. 串口那行 `ESP response: ERROR` 用户观察到周期约 **2-3 秒**，但代码里重连冷却是 **5 秒**（`TCP_RECONNECT_COOLDOWN_MS`）。这个对不上，暂未解释。

---

## 接下来要做什么

按"确定性"排序，先做最靠得住的。

### 第一层：节点自愈看门狗（**根治，且完全不依赖网络**）

原理：**`AT+RST` 会把 ESP 的 ARP 表、socket、AT 状态全部清空重建。** 所以不管触发是什么，重启 ESP 一定能拉回来，而且 100% 在节点自己手里。

- [ ] **换判据**：把 `tcp_connected` 换成"连续 N 次收不到 ACK"（建议 `N = 5`，收到任意一个 ACK 就清零）。现在 `tcp_connected` 只在 AT 握手失败时才变 0，而 ARP 卡住时 `AT+CIPSEND` 是成功的 —— **重连逻辑从头到尾就没被触发过**。
- [ ] **L1（~1~5 秒，不重启 ESP）**：`AT+CIPCLOSE` → `AT+CIPSTART`。顺带修掉"ESP 回 `ALREADY CONNECTED` 导致永远连不上"——解析器用完整相等比较，匹配不上 `CONNECT`。
- [ ] **L2（~10~20 秒断流，积压进 pending）**：`AT+RST` → 等 `ready` → 等 `WIFI GOT IP` → 重跑 `CIPMUX` / `CIPMODE` / `CIPSTART`。需要给"等 `WIFI GOT IP`"设超时上限（建议 30 秒），避免万一 ESP 一直不入网时任务永远卡住。
- [ ] **L1 连败 3 次才升到 L2**，L2 失败就冷却后重试，不做无脑重启。
- [ ] `tcp_init()` 里补一条 `AT+CWAUTOCONN=1`（ESP 默认值，但显式设一次，保证 `AT+RST` 后能自动重新入网）。
- [ ] `transmit_task.c:179` 现在是 `(void)tcp_init(...)`，**返回值被丢掉了** —— 初始化失败没人知道。

> `AT+CIPCLOSE`（`esp12s.h:56`）和 `AT+RST`（`esp12s.h:13`）都定义了，**全工程一次都没调用过**。

### 第二层：Hub 拆槽位泄漏（**确定有用，独立成立**）

这是"只有重启 Hub 才行"的解药，跟触发原因无关，是纯粹的 bug。

- [ ] `SO_KEEPALIVE`，调到 30/10/3（默认是 2 小时才探活，必须调）。
- [ ] 事件循环里加空闲巡检：超过 60 秒没活动的连接直接关。
- [ ] 同 node 已有连接时，新连接踢掉旧的（`handleTcpServe()` 现在不检查）。

### 第三层：Hub 定时保活帧（**廉价保险，效果未证实**）

- [ ] 每条空闲连接每 30 秒发一个小帧（16 字节，复用现有帧长，node 侧不用改）。理论上能让内核走 `STALE → DELAY → PROBE` 从而自动发出真正的 ARP 请求，把 20 分钟的过期周期压到几十秒。
- [ ] **但见上文"还没解释的两处"第 1 条** —— 正常流量为什么没能阻止它，我还没搞明白，所以这条只当保险。

### 诊断缺口（建议顺手补上，不改行为）

- [ ] `tcp_wait_prompt()` 和 `tcp_wait_send_ok()` **失败时一个字都不打**（只有 `tcp_wait_response()` 打，就是 `tcp.c:73` 那句 `ESP response: ERROR`）。所以真正把 `tcp_connected` 打成 0 的那一下，串口是哑的；我们看到的 ERROR 是**事后**重连循环打的**结果**，不是原因。
- [ ] 在这两个函数的每个失败分支各加一行打印，并在 `tcp_data_send()` 把 `tcp_connected` 置 0 的两处各打一行。**下次故障串口会直接报出 ESP 到底回了什么**（`BUSY` / `TIMEOUT` / `ERROR` 指向三个完全不同的方向）。

### 顺带记下的、暂不处理的

- [ ] `log_scan_log()` 上电全片扫描耗时几十秒到一分多钟（见上一节）。同类问题的 `log_find_next_pending` 早退补丁已经在未提交的改动里了，上电路径还没修。
- [ ] CAN 链路：重连不补发、被 TCP 重连堵塞导致 pending 上涨（见前面"阶段性总结"）。