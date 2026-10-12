# 2026-10-11

## 1.今天先把TCP冻结的事情给修复了

我们来回顾一下这个bug，我们发现，基本上每20分钟就会断链一次。这里的“断链”是指Hub不再收到新的上报数据，但Hub上仍然能看到原来的TCP连接，
Node串口也会持续打印`ESP response: ERROR`。所以当时不是整个程序都卡死了，而是TCP业务停止了，重连逻辑还在运行。
排查过程中还发现，ping节点后，上报有时会恢复。后来做了单独发送ARP请求的实验，也观察到了恢复现象。这说明网络侧的状态值得继续调查，
但目前还不能确定就是ESP内部的ARP表过期导致的，最初为什么断流仍然需要验证。
不过，从代码中找到了两个明确的恢复缺陷。第一，原来的重连依赖`tcp_connected`标志，但ESP返回`SEND OK`，不代表Hub已经收到并处理了数据。
如果一直收不到Hub的业务ACK，而连接标志仍然为1，就不会触发重连。
第二，连接标志变成0以后，原来的恢复只会重复发送`AT+CIPSTART`。如果ESP内部还保留着旧连接，或者内部状态异常，重复建立连接可能一直失败，
程序没有进一步清理和重启ESP的手段。
这次修改增加了周期恢复检查，即使没有新的发送任务，也会推进恢复。连续5次收不到匹配的TCP业务ACK，就进入恢复流程；先关闭旧连接再重连，3次仍未恢复业务通信，
就通过`AT+RST`重启ESP并重新联网。只有收到匹配节点和序号的TCP业务ACK，才清零普通重连尝试次数；每轮恢复开始时，会重新统计本轮上报失败次数。
恢复过程采用分步执行，等待ESP回应时还能继续处理CAN。当前模拟测试和默认、A/B槽编译均已通过，接下来需要烧录并长时间运行，验证实际冻结后能否自动恢复。
因此，现在完成的是自愈代码，还不能直接认定实际故障已经彻底解决。
这样做有两个目的，一是让TCP出问题后能自己恢复，二是避免恢复过程中一直等待ESP回应，把同一个任务里的CAN通信也拖住。
不过要注意，首次初始化和普通TCP发送仍然有原来的等待，这次并没有解决所有阻塞问题。另外，AT发送交互本身失败时会直接把TCP标记为不可用，不需要等满5次业务ACK失败。

### 现在我们来想一下该怎么写代码

我们不能在重连函数里面一直等ESP，而是发送一条命令后先返回，下一次调用时再看回应来了没有。这样需要有一个变量，记住当前处于重连的什么阶段，这就是状态机。

我们先定义一个枚举，每一个阶段都有一个对应的描述：

```c
typedef enum
{
    TCP_RECOVERY_IDLE,    // 空闲，没有恢复命令正在执行
    TCP_RECOVERY_CLOSE,   // 关闭旧TCP连接
    TCP_RECOVERY_CONNECT, // 建立到Hub的TCP连接
    TCP_RECOVERY_RESET,   // 重启ESP
    TCP_RECOVERY_STATION, // 设置Wi-Fi工作模式
    TCP_RECOVERY_JOIN,    // 加入Wi-Fi
    TCP_RECOVERY_MUX,     // 设置单连接模式
    TCP_RECOVERY_MODE     // 设置普通数据发送模式
} tcp_recovery_state_t;
```

这里有一个我前面理解错的地方，IDLE不是未连接，而是没有恢复命令正在执行。TCP正常时可以是IDLE，TCP断了正在等下次重试时也可以是IDLE。
是否允许尝试TCP发送看的是tcp_connected，而不是只看recovery_state。
枚举也不会让程序自动从上往下执行，后面的switch才决定下一步走哪里。

接下来我们需要记录当前的步骤、时间、失败次数等：

```c
static tcp_recovery_state_t recovery_state; // 当前恢复阶段
static uint32_t recovery_started_ms;       // 当前步骤或冷却开始的时间
static uint32_t recovery_wait_ms;          // 当前步骤超时长度，或空闲时的重试间隔
static uint8_t recovery_attempts;          // 尚未得到业务ACK确认的普通重连尝试次数
static uint8_t recovery_ack_misses;        // 本轮连续失败的TCP上报次数
static uint8_t recovery_connect_seen;      // 是否收到CONNECT，0没有，1有
static uint8_t recovery_ok_seen;           // 是否收到OK，0没有，1有
```

uint32_t用来存毫秒时间，uint8_t用来存次数或0/1标志。这些变量定义在函数外面，函数返回后也会保留。
这里static的作用是把变量限制在tcp.c内部使用，不是加了static它们才不会在函数返回后消失。
它们没写初值时自动为0，所以recovery_state最初对应IDLE。

### 现在我们来开始描述service需要怎么做

#### 1.先检查配置，再取ESP的回应

```c
uint8_t tcp_recovery_service(void)
{
    esp12s_response_t response;
    uint8_t finished = 0U;
    uint8_t count;
    tcp_recovery_state_t next;

    if(tcp_saved_config == NULL)
    {
        return TCP_FAIL;
    }
    /* 限制每轮消费数量，保持 CAN/OTA 队列能够继续处理。 */
    for(count = 0U; count < 16U; count++)
    {
        if(esp12s_response_get(&response) == 0U)
        {
            break;
        }
```

上面是函数开头的片段，for循环后面的代码我们接着往下看。

tcp_saved_config保存首次初始化时校验过的Wi-Fi和Hub配置，没有配置就不能拼接连接命令，所以直接返回。
response用来存本次取出的ESP回应，finished表示当前步骤是否完成，next用来存下一步要做什么。
finished是局部变量，每次调用重新从0开始；而recovery_connect_seen和recovery_ok_seen会保留之前已经收到的回应。

ESP的串口回应先由中断识别，再放入回应队列，service从队列取出处理。这里取的是AT回应，不是Hub的业务ACK，两个不能混在一起。
esp12s_response_get返回0说明现在队列空了，break退出for循环。每次最多取16条，不是必须等16条，也不会因为少了几条就一直等。

这里顺便记一下三个容易搞混的语句：

- break：退出当前循环，继续执行循环后面的代码。
- continue：跳过当前这一轮，进入下一轮循环。
- return：退出整个函数，回到调用它的位置。

#### 2.根据当前阶段理解ESP回的话

```c
        if(response == ESP12S_RESPONSE_WIFI_CONNECTED)
        {
            wifi_connected = TCP_SUCCESS;
        }
        if(recovery_state == TCP_RECOVERY_IDLE)
        {
            if(response == ESP12S_RESPONSE_READY)
            {
                wifi_connected = TCP_FAIL;
            }
            if((response == ESP12S_RESPONSE_CLOSED) || (response == ESP12S_RESPONSE_READY))
            {
                tcp_connected = TCP_FAIL;
            }
            continue;
        }
        if(recovery_state == TCP_RECOVERY_RESET)
        {
            if(response == ESP12S_RESPONSE_READY)
            {
                finished = 1U;
            }
            continue;
        }
        /* 无连接时 CIPCLOSE 可回 ERROR；超时则不当作关闭成功。 */
        if((recovery_state == TCP_RECOVERY_CLOSE) &&
           (response == ESP12S_RESPONSE_ERROR))
        {
            finished = 1U;
            continue;
        }
        if((response == ESP12S_RESPONSE_ERROR) || (response == ESP12S_RESPONSE_FAIL) ||
           (response == ESP12S_RESPONSE_BUSY) ||
           ((response == ESP12S_RESPONSE_CLOSED) && (recovery_state != TCP_RECOVERY_CLOSE)))
        {
            tcp_recovery_failed();
            return TCP_FAIL;
        }
        if(response == ESP12S_RESPONSE_CONNECT) recovery_connect_seen = 1U;
        if(response == ESP12S_RESPONSE_OK) recovery_ok_seen = 1U;
        if((recovery_ok_seen != 0U) &&
           ((recovery_state != TCP_RECOVERY_CONNECT) || (recovery_connect_seen != 0U)))
        {
            finished = 1U;
        }
    }
```

首先，如果收到WIFI CONNECTED，就把wifi_connected记为成功。这只是Wi-Fi状态，不代表已经连接Hub。

如果当前为IDLE，就处理ESP主动发来的通知。CLOSED表示TCP连接关闭，把tcp_connected标为失败；READY表示ESP启动完成，原来的Wi-Fi和TCP状态不能继续相信，先把两者标为失败。
最后continue去处理下一条回应，不再进入下面的通用成功、失败判断。

如果当前为RESET，就只等READY。AT+RST后可能先收到OK，但OK不代表ESP已经启动完成，收到READY才把finished置1。
其他回应在这个阶段先忽略，始终等不到READY的话，由后面的超时检查结束本次恢复。

如果当前为CLOSE，收到ERROR也允许结束关闭步骤，因为没有连接可关时也可能回ERROR。这不是证明关闭成功，只是选择继续尝试建立连接；如果后续仍失败，就再走恢复。
CLOSE阶段收到CLOSED本身也不会当成错误，但当前实现仍然等OK或前面特殊处理的ERROR来结束关闭步骤。

其他阶段收到ERROR、FAIL、BUSY，或者非关闭阶段收到CLOSED，就调用tcp_recovery_failed，结束本次恢复。

最后的判断有点绕，我们把意思拆开：普通配置命令收到OK就算当前步骤完成，CONNECT阶段则必须同时收到CONNECT和OK。
两个回应可能分两次到达，所以用两个seen变量记住。比如这次收到CONNECT，就先记为1；下次收到OK，两项都齐了，finished才变为1。
finished变成1只是表示当前步骤可以结束，下一条命令要到for循环后面才会发送。

#### 3.当前没有恢复命令时，判断是否开始恢复

```c
    if(recovery_state == TCP_RECOVERY_IDLE)
    {
        if((tcp_connected == TCP_FAIL) &&
           ((uint32_t)(board_systick_ms - recovery_started_ms) >= recovery_wait_ms))
        {
            if(recovery_attempts < 3U)
            {
                recovery_attempts++;
                next = TCP_RECOVERY_CLOSE;
            }
            else
            {
                wifi_connected = TCP_FAIL;
                next = TCP_RECOVERY_RESET;
            }
            recovery_ack_misses = 0U;
            if(tcp_recovery_command(next) == TCP_FAIL) tcp_recovery_failed();
        }
        return TCP_FAIL;
    }
```

这里的两个条件必须同时满足：TCP被标记为不可用，并且已经达到重试时间。
board_systick_ms是系统运行的毫秒计数，用当前时间减去开始时间，就得到经过了多久。
例如从10000毫秒开始，要求等5000毫秒，当前是13000就先返回，到了15000才允许开始。这个比较不让程序在这里停5秒，而是下次调用再看。

如果recovery_attempts还不到3，先加1，再选择CLOSE。这次先关闭旧连接，关闭结束后才会进入CONNECT。
如果已经尝试3次还没有业务ACK确认，就选择RESET，后面重启ESP并重新配置Wi-Fi。
这里的3次不是指重启3次，而是普通重连尝试次数；即使CIPSTART成功，只要业务ACK没成功确认，attempts也不会清零。

recovery_ack_misses在新一轮恢复开始时清零，让后续上报重新统计。注意attempts没有清零，否则每次都会回到第1次，永远无法升级恢复。
选好next以后调用tcp_recovery_command发送对应命令。它失败时调用tcp_recovery_failed安排后续重试。

最后return结束本次service调用。因此本次刚开始关闭或者重启时，不会紧接着执行下面的switch，这就是分步的一部分。
即使条件不满足，没有发命令，也会返回。这里返回TCP_FAIL只表示这次没有完成新连接建立，不能把它一律理解为本次重连失败。

#### 4.当前步骤没完成，就检查超时后返回

```c
    if(finished == 0U)
    {
        if((uint32_t)(board_systick_ms - recovery_started_ms) >= recovery_wait_ms)
        {
            tcp_recovery_failed();
        }
        return TCP_FAIL;
    }
```

没有回应或者回应还没凑齐，finished就还是0。没超时直接返回，下次再检查；超时则调用失败处理后返回。
这里没有while一直等ESP，这样同一个发送任务才有机会继续处理其他事情。

#### 5.当前步骤完成，决定下一步

```c
    switch(recovery_state)
    {
        case TCP_RECOVERY_CLOSE: next = TCP_RECOVERY_CONNECT; break;
        case TCP_RECOVERY_RESET: next = TCP_RECOVERY_STATION; break;
        case TCP_RECOVERY_STATION: next = TCP_RECOVERY_JOIN; break;
        case TCP_RECOVERY_JOIN: next = TCP_RECOVERY_MUX; break;
        case TCP_RECOVERY_MUX: next = TCP_RECOVERY_MODE; break;
        case TCP_RECOVERY_MODE: next = TCP_RECOVERY_CONNECT; break;
        case TCP_RECOVERY_CONNECT:
            recovery_state = TCP_RECOVERY_IDLE;
            recovery_started_ms = board_systick_ms;
            recovery_wait_ms = 5000U;
            tcp_connected = TCP_SUCCESS;
            printf("[tcp recovery] connected; awaiting business ACK\r\n");
            return TCP_SUCCESS;
        default: return TCP_FAIL;
    }
    if(tcp_recovery_command(next) == TCP_FAIL) tcp_recovery_failed();
    return TCP_FAIL;
}
```

普通恢复是CLOSE到CONNECT，升级恢复则是RESET到STATION、JOIN、MUX、MODE，最后CONNECT。
选择next以后发出下一条命令，然后返回；不能把这个switch理解成所有case依次执行，每个case都有break，只选择当前状态对应的那一条。

CONNECT完成后，把状态改为IDLE，并允许TCP发送，返回TCP_SUCCESS告诉发送任务，这次有新连接可以尝试补发了。
这里没有清零attempts，因为建立连接还不能证明Hub已经处理了业务数据。
连接完成时也会设置5秒的计时起点，之后如果再次失败，还要满足重试时间条件才能发起下一轮恢复。

#### 6.tcp_recovery_command是怎么发送下一步的

```c
static uint8_t tcp_recovery_command(tcp_recovery_state_t state)
{
    const char *command = NULL;
    int length = 0;

    switch(state)
    {
        case TCP_RECOVERY_CLOSE: command = ESP_AT_CIPCLOSE; break;
        case TCP_RECOVERY_RESET: command = ESP_AT_RST; break;
        case TCP_RECOVERY_STATION: command = "AT+CWMODE=1\r\n"; break;
        case TCP_RECOVERY_MUX: command = "AT+CIPMUX=0\r\n"; break;
        case TCP_RECOVERY_MODE: command = "AT+CIPMODE=0\r\n"; break;
        case TCP_RECOVERY_JOIN:
            length = snprintf(tcp_command_buffer, sizeof(tcp_command_buffer),
                              "AT+CWJAP=\"%s\",\"%s\"\r\n",
                              tcp_saved_config->wifi_ssid, tcp_saved_config->wifi_password);
            command = tcp_command_buffer;
            break;
        case TCP_RECOVERY_CONNECT:
            length = snprintf(tcp_command_buffer, sizeof(tcp_command_buffer),
                              "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n",
                              tcp_saved_config->server_ip,
                              (unsigned int)tcp_saved_config->server_port);
            command = tcp_command_buffer;
            break;
        default: return TCP_FAIL;
    }
    if((length < 0) || ((uint32_t)length >= sizeof(tcp_command_buffer)))
    {
        return TCP_FAIL;
    }
    recovery_state = state;
    recovery_started_ms = board_systick_ms;
    recovery_wait_ms = (state == TCP_RECOVERY_JOIN) ? TCP_WIFI_JOIN_TIMEOUT_MS :
                       (state == TCP_RECOVERY_RESET) ? 15000U :
                       (state == TCP_RECOVERY_CONNECT) ? TCP_RECONNECT_TIMEOUT_MS : TCP_AT_TIMEOUT_MS;
    recovery_connect_seen = 0U;
    recovery_ok_seen = 0U;
    esp12s_response_reset();
    esp12s_cmd_send(command);
    printf("[tcp recovery] stage=%u attempt=%u\r\n", (unsigned int)state,
           (unsigned int)recovery_attempts);
    return TCP_SUCCESS;
}
```

这个函数前面的switch根据state选命令。JOIN要填Wi-Fi账号密码，CONNECT要填Hub的IP和端口，因此使用snprintf把参数写进tcp_command_buffer。
length是snprintf的返回值，小于0或者达到缓冲区大小，就说明这条命令没准备好，不能继续发送。
command指向最终要发送的字符串，它本身不是发送动作。

准备好命令后，先把state保存到recovery_state，记录当前毫秒时间，再设置这一步最多等多久：JOIN等50秒，RESET等15秒，CONNECT等5秒，其他步骤等10秒。
这里的等待长度是超时上限，不是每次调用都会等满这些时间。

然后清除上一条命令的CONNECT和OK记录，清除旧AT回应，再把命令通过串口发给ESP。函数不等待ESP回应，发完就返回。
因此这里TCP_SUCCESS的意思是命令已经准备并发送，不是ESP已经执行成功，也不是Hub已经收到数据。
ESP后面返回什么，仍由service下次调用时判断。

#### 7.失败之后怎么处理

```c
static void tcp_recovery_failed(void)
{
    printf("[tcp recovery] failed stage=%u; retry later\r\n", (unsigned int)recovery_state);
    recovery_state = TCP_RECOVERY_IDLE;
    tcp_connected = TCP_FAIL;
    recovery_started_ms = board_systick_ms;
    recovery_wait_ms = (recovery_attempts >= 3U) ? 30000U : 5000U;
}
```

失败时回到IDLE，把TCP标记为不可用，从当前时间重新开始冷却。
attempts不到3就等5秒，已经达到3就等30秒，避免一直失败时频繁重启ESP。
这里保留attempts，所以下一次才能继续升级恢复。这个函数也不等待，设置好变量就返回。

#### 8.怎样确认通信真的恢复了

```c
void tcp_recovery_note_ack(uint8_t success)
{
    /* 跳过恢复期间的 CAN 回退，只有实际尝试 TCP 的结果才由调用方上报。 */
    if(success != 0U)
    {
        if((recovery_attempts != 0U) || (recovery_ack_misses != 0U))
        {
            printf("[tcp recovery] business ACK verified\r\n");
        }
        recovery_attempts = 0U;
        recovery_ack_misses = 0U;
    }
    else
    {
        if(recovery_ack_misses < 5U)
        {
            recovery_ack_misses++;
        }
        if(recovery_ack_misses >= 5U)
        {
            tcp_connected = TCP_FAIL;
        }
    }
}
```

这个函数由发送任务在实际尝试TCP上报后调用。成功必须是收到了节点、seq和状态都匹配的业务ACK，不是ESP返回SEND OK。
成功就清零attempts和ack_misses，说明业务已经得到确认。
失败就累计本轮上报失败次数，最多记到5。达到5次把tcp_connected改为失败，让service后面进入恢复。
如果AT发送本身就失败，发送函数会直接把tcp_connected改为失败，因此不一定要累计满5次才开始恢复。

恢复期间TCP不可用，就跳过普通TCP发送，数据尝试走CAN。CAN成功不能给这个函数传成功，因为CAN只能证明CAN正常，不能证明TCP恢复。
如果暂时没有任何TCP上报尝试，也不会凭空累计ACK失败；但已进入的恢复流程仍然会由service推进。

#### 9.为什么放在同一个函数里，仍然叫分步执行

发送任务每轮循环先调用service，再处理OTA和上报。它不依赖这轮有没有拿到新的上报数据，也没有新增一个专门重连的任务。
service返回TCP_SUCCESS时设置link_restored，让LogTask尝试补发pending。这里的补发触发不是业务完成确认，记录仍然需要有效ACK才能确认。

我们把一次普通重连按调用顺序走一遍：

| 哪次调用 | 当前情况 | 这次做什么 |
| --- | --- | --- |
| 第1次 | IDLE，TCP失败，时间到了 | 保存CLOSE状态，发CIPCLOSE，然后返回 |
| 第2次 | CLOSE，没有回应，没超时 | 返回，让发送任务处理其他事情 |
| 第3次 | CLOSE，收到关闭步骤结果 | 切换CONNECT，发CIPSTART，然后返回 |
| 后续某次 | CONNECT，只收到CONNECT | 记住CONNECT已经收到，返回 |
| 后续某次 | CONNECT，又收到OK | 连接步骤完成，允许TCP发送，返回成功 |
| 后续上报 | 收到匹配的业务ACK | 清零attempts，确认业务恢复 |

这些调用次数只是为了说明流程，不是固定第几轮必须收到回应。如果两条回应都在队列里，一次调用也可以一起处理。

代码写在同一个函数里，不代表一次调用会从头做到尾。关键是用recovery_state保存进度，等待回应时return，把执行机会交还给发送任务。
同时要区分三个成功：命令发出、TCP连接建立、业务ACK确认，这三个不是一回事。

## 2.现在我们处理edgehub的槽位泄露问题

先想一下为什么会有cilent的槽位泄露
现有代码中，我们给每一个客户端分配了一个client槽位

```cpp
struct ClientState
{
    explicit ClientState(int fd)
        : connection(fd), receive_ringbuffer(RINGBUFFER_LENGTH)
    {
    }

    TcpConnection connection;
    Ringbuffer receive_ringbuffer;
};
```

我们的初衷是为了给每一个客户端一个环形缓冲区和连接描述并通过closeTcpclient来将客户端槽位进行释放
问题就出在了这里，现在唯一会调用closeTcpclient在

```cpp
   if(close_client)
    {
        closeTcpclient(fd, slot);
    }
```

而close_client置true的情况包括

- 收到EPOLLRDHUP或EPOLLHUP，说明连接出现关闭。
- recv()返回0，说明对端正常关闭了发送方向。
- 接收出现不可恢复的错误。
- 环形缓冲区处理失败、解析出现错误，或者业务ACK没有完整发出。

这些情况下，最终都会调用closeTcpclient()，完成epoll登记删除、fd类型登记删除和客户端状态销毁。
在昨天的测试中，他暴露了一种没有考虑到的特殊情况
比如Node突然断电或者Wi-Fi异常，此时它可能没有机会向Hub发送关闭通知。Hub暂时不知道对方已经无法通信，原来的连接就会继续保留在clients数组中。
当Node恢复后重新连接Hub，Hub会为新连接分配一个新的槽位。但新旧连接是两个不同的socket，现有代码没有记录它们对应的节点号，也就不知道这个新连接实际上是在替换原来的连接。
这样就可能出现一个节点占用多个槽位的情况。旧连接迟迟没有清理，新连接又不断加入，最终占满MAX_CLIENTS。此时即使Node能够重新发起连接，Hub也会因为找不到空槽位而拒绝它。

### 现在我们来想一下如何修复

出现这个问题的原因主要是因为每个client的描述太弱，我们需要增加一个描述保存最后收到有效遥测的时间。
那我们就可以用当前时间-最后遥测时间算出来的就是这个客户端有多久没有发送信息了
如果这个时间过长我们就可以关闭这个槽位，毕竟后续如果客户端又开始发送我们再复用现有流程进行连接，不会出现槽位泄露的问题

我们在epoll的每次循环的最后去扫描整个client，要是又超时的，我们就close，这样就算完成了,改完后就是这样

```cpp
bool Gateruntime::run(volatile sig_atomic_t *g_running)
{
    struct epoll_event events[EPOLLEVENT_SIZE];
    while(*g_running != 0)
    {
        int timeout_ms = 1000U;
        if(_ota.isSending()) timeout_ms = 1;
        else if(_ota.hasPendingQueries()) timeout_ms = 100;
        int nready = _epoll.wait(events, EPOLLEVENT_SIZE, timeout_ms);
        if(nready == -1)
        {
            //修复：EINTR只是epoll_wait被信号临时中断，重新等待就可以；其他错误才说明事件循环无法继续
            if(errno == EINTR)
            {
                continue;
            }
            perror("epoll error");
            return false;
        }

        for(int event_index = 0;event_index < nready;event_index++)
        {
            int fd = events[event_index].data.fd;
            dispatchEvent(fd, events[event_index].events);
        }

        closeTimeoutClient();

        OTA::Result result;
        if(_ota.serviceSend(result))
            completeHttpRequest(result);
        while(_ota.pollTimeout(result))
            completeHttpRequest(result);
        // CAN 回调只准备响应，当前事件批次结束后统一推进 HTTP 发送。
        for(int slot = 0; slot < MAX_CLIENTS; ++slot)
        {
            if(httpclients[slot] && !httpclients[slot]->response.empty())
                handleHttpClient(httpclients[slot]->connection.fd(), 0U);
        }
    }

    return true;
}

void Gateruntime::closeTimeoutClient()
{
    //这个函数来实现遍历client然后分别找每一个slot的last_valid_ms是否大于某个数，然后关闭
    uint64_t now = tcp_now_ms();
    for(int slot = 0;slot < MAX_CLIENTS;slot++)
    {
        if(clients[slot] == nullptr)
        {
            continue;
        }

        uint64_t idle_ms = now - clients[slot]->last_vaild_ms;

        if(idle_ms > MAX_CLIENT_IDLE_MS)
        {
            int fd = clients[slot]->connection.fd();
            closeTcpclient(fd, slot);
        }
    }
}
```

注意我们这里还修改了epoll的最大等待时间从-1变为1000(timeout_ms)
原本-1是无限等待直到有时间，但是现在循环还要处理关闭客户端，所以当没有链接的时候我们也需要往下走让他去关闭旧连接

## 3所有都做好以后我们尝试启动，结果却发现TCP完全上报不了，但是可以连接到wifi并获取IP

```text
[16:07:58.326] [transmit] seq=80084 temp=2888 scale=-2 result=FAIL
[16:07:58.420] [tcp recovery] stage=3 attempt=3
[16:07:58.684] [tcp recovery] stage=4 attempt=3
[16:07:58.696] [tcp recovery] stage=5 attempt=3
[16:07:59.326] [collect] seq=80085 temp=2886 scale=-2 uptime=491537
[16:07:59.333] [log] seq=80085 temp=2886 scale=-2 addr=0x138D50
[16:07:59.337] [transmit] seq=80085 temp=2886 scale=-2 result=FAIL
[16:08:00.338] [collect] seq=80086 temp=2886 scale=-2 uptime=492548
[16:08:00.344] [log] seq=80086 temp=2886 scale=-2 addr=0x138D60
[16:08:00.349] [transmit] seq=80086 temp=2886 scale=-2 result=FAIL
[16:08:01.348] [collect] seq=80087 temp=2884 scale=-2 uptime=493559
[16:08:01.355] [log] seq=80087 temp=2884 scale=-2 addr=0x138D70
[16:08:01.359] [transmit] seq=80087 temp=2884 scale=-2 result=FAIL
[16:08:02.360] [collect] seq=80088 temp=2887 scale=-2 uptime=494570
[16:08:02.366] [log] seq=80088 temp=2887 scale=-2 addr=0x138D80
[16:08:02.370] [transmit] seq=80088 temp=2887 scale=-2 result=FAIL
[16:08:03.371] [collect] seq=80089 temp=2885 scale=-2 uptime=495581
[16:08:03.376] [log] seq=80089 temp=2885 scale=-2 addr=0x138D90
[16:08:03.381] [transmit] seq=80089 temp=2885 scale=-2 result=FAIL
[16:08:03.706] [tcp recovery] stage=6 attempt=3
[16:08:03.716] [tcp recovery] stage=7 attempt=3
[16:08:03.728] [tcp recovery] stage=2 attempt=3
[16:08:04.382] [collect] seq=80090 temp=2887 scale=-2 uptime=496592
[16:08:04.388] [log] seq=80090 temp=2887 scale=-2 addr=0x138DA0
[16:08:04.393] [transmit] seq=80090 temp=2887 scale=-2 result=FAIL
[16:08:05.392] [collect] seq=80091 temp=2885 scale=-2 uptime=497603
[16:08:05.399] [log] seq=80091 temp=2885 scale=-2 addr=0x138DB0
[16:08:05.403] [transmit] seq=80091 temp=2885 scale=-2 result=FAIL
[16:08:06.403] [collect] seq=80092 temp=2886 scale=-2 uptime=498614
[16:08:06.411] [log] seq=80092 temp=2886 scale=-2 addr=0x138DC0
[16:08:06.415] [transmit] seq=80092 temp=2886 scale=-2 result=FAIL
[16:08:06.913] [tcp recovery] failed stage=2; retry later
[16:08:07.415] [collect] seq=80093 temp=2885 scale=-2 uptime=499625
[16:08:07.421] [log] seq=80093 temp=2885 scale=-2 addr=0x138DD0
[16:08:07.425] [transmit] seq=80093 temp=2885 scale=-2 result=FAIL
[16:08:07.977] stack free words: log=387 collect=397 transmit=367 led1=231 led2=230 hmi=252 monitor=396
```

我总感觉是树莓派的无线网卡有问题，我们来尝试将
这部分偏复杂，详情可见同目录的另外一篇md
最后尝试运行，TCP通信恢复，视频在//video下

现在又遇到一个LS2K0300运行一段时间自动关闭，这个不知道什么原因还在测试
现在又稳定了，玄学吧可能，不管了

运行了大约2个小时，TCP冻结未发生，初步判断修复完毕，接下来我们稍微弄一下edgedash，本身就是个仪表盘，我们的做法是吧hub现有的http接口
原本是给本机crul的，现在只要edgedash也用同样的请求发送给hub，hub回应后可以显示数据，作为仪表盘，它不需要很复杂
最主要是UI好看（笑）
