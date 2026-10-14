# EdgeHub

EdgeLink 的 Linux 网关，使用 C++17、边沿触发 `epoll`、SocketCAN 和 SQLite。接收 Node 的 TCP/CAN 遥测，校验后落库或去重，再回复业务 ACK；HTTP 接口用于指定节点的 CAN/TCP 槽位查询和固件下发。

本文件负责网关的构建、部署、HTTP 操作、线协议和排障。整体架构与特殊处理见[项目 README](../README.md)，节点配置与烧录见 [EdgeNode README](../edgenode/README.md)。

## 目录与职责

| 文件 / 目录 | 职责 |
| --- | --- |
| [main.cpp](main.cpp) | 信号处理、初始化与运行入口；初始化失败立即退出 |
| [Gateruntime.cpp](src/Gateruntime.cpp) | 拥有监听器、连接、fd 登记、数据库与 OTA 对象，分发事件并统一清理连接 |
| [Tcp.cpp](src/Tcp.cpp)、[Can.cpp](src/Can.cpp)、[Epoll.cpp](src/Epoll.cpp) | socket、SocketCAN 与 epoll 访问 |
| [Ringbuffer.cpp](src/Ringbuffer.cpp)、[TcpFrame.cpp](src/TcpFrame.cpp) | 每连接字节缓冲与帧解析，处理半包/粘包 |
| [Message.cpp](src/Message.cpp)、[Storage.cpp](src/Storage.cpp) | 统一业务模型，SQLite 插入/去重结果 |
| [Http.cpp](src/Http.cpp)、[HttpHandle.cpp](src/HttpHandle.cpp) | HTTP 请求解析与路由意图，不阻塞等待整场 OTA |
| [OTA.cpp](src/OTA.cpp) | CAN 查询/窗口与 TCP 分块状态、文件读取、确认和有限重试 |
| [tools/](tools/README.md)、[cmake/](cmake/) | 随仓库构建的 SQLite 与 LoongArch 工具链配置 |

## 运行配置

| 项目 | 当前值 | 定义位置 / 注意事项 |
| --- | --- | --- |
| 遥测 TCP | `8888` | [Gateruntime.hpp](inc/Gateruntime.hpp) |
| 管理 HTTP | `8080` | 同上 |
| CAN 接口 | `can0`，项目使用 500 kbit/s | [Can.cpp](src/Can.cpp)；接口由系统配置，build.sh 不改总线状态 |
| SQLite | `data/edgehub.db` | 相对程序工作目录；父目录必须存在且可写 |
| 连接槽位 | TCP 与 HTTP 各最多 20 个 | `MAX_CLIENTS`；不是节点 ID 上限 |
| 节点编号 | 1～127 | 同一 Hub 下不能重复，Node 配置必须指向当前 Hub |

当前初始化要求 `can0` 存在并能够绑定，即使只准备测试 TCP/HTTP，也不能假设程序会跳过 CAN 初始化。`/hub/status` 只说明 HTTP 服务可响应，不证明 Node 已在线、数据库写入成功或 OTA 已完成。

## 构建与运行

### Linux 本机

需要 Linux、CMake、C/C++17 编译器及 SocketCAN 支持。SQLite amalgamation 随仓库直接编译，无需目标平台单独安装 SQLite3 开发包。

在实际运行 Hub 的机器上配置 CAN；已按正确波特率启动的接口无需重复配置：

```bash
sudo ip link set can0 up type can bitrate 500000
```

在仓库根目录进入网关目录：

```bash
cd edgehub
mkdir -p data
./build.sh --native --run
```

只检查原生编译可运行 `./build.sh --native`。本机模式使用当前机器的编译器；程序产物为 `build/Edgehub`。Ctrl+C 正常停止前台程序，Ctrl+Z 会暂停进程并保留端口占用。

### LS2K0300 交叉编译与部署

在 WSL/Linux 安装配套 LoongArch 工具链，可用 `LS2K0300_TOOLCHAIN` 覆盖默认位置，详见[第三方依赖与工具链说明](tools/README.md)。以下命令均在 `edgehub/` 执行：

```bash
# 默认仅交叉编译，不启动服务
./build.sh

# 示例板子 IP，按实际部署替换；以下运行方式选择一种
./build.sh --deploy 192.168.1.4 --run
./build.sh --deploy 192.168.1.4 --bg

# 查看后台日志，不重新编译或部署
./build.sh --logs 192.168.1.4
```

部署使用 `root` 用户，把程序复制到 `/home/root/Edgehub`，在 `/home/root` 启动并创建 `data/`。后台日志为 `/tmp/Edgehub.log`，PID 文件为 `/tmp/Edgehub.pid`。部署会先停止旧 Edgehub，再上传新程序；它会中断已有连接。脚本不配置板上 `can0`，需要在板上单独准备。

原生与交叉编译共用 `build/`，切换模式时脚本会清理这个构建目录并重新配置。不要将需要保留的文件放入 `build/`。

## 事件循环和发送边界

- ET 模式下持续 `accept`、`recv` 和 CAN `read` 到 `EAGAIN`；每个 TCP 客户端独立保存 RingBuffer、节点身份和输出进度。
- 遥测通过校验后交给 Storage。`Inserted` 与 `Duplicate` 都回复成功 ACK，`Error` 不回成功 ACK；`receivedAtUs` 是 Hub 的接收时间，不是 Node 的采样 uptime。
- `sendFirmware()` / `sendFirmwareTcp()` 成功只表示文件检查通过、发送状态建立；事件循环随后推进传输，不能将启动成功当成 Node 已接收。
- TCP 固件帧、查询和遥测 ACK 共用连接输出缓冲，部分发送后保留偏移，遇到 `EAGAIN` 等可写事件。整帧提交 socket 后才开始等 Node ACK。
- HTTP 用 `request_id` 关联结果；TCP 固件使用另一个线上 `transfer_id`。连接关闭会结束绑定的 TCP 下发，不能把旧传输迁移到重连后的连接。
- 连接清理集中执行 epoll 删除、fd 登记移除和连接对象释放，避免不同错误分支留下失效状态。

## HTTP 管理与固件下发

HTTP 服务监听 `8080`。节点号使用十进制 1～127，槽位查询与固件下发是两个独立操作，由调用者查询后选择对应镜像。

| 方法 | 路径 | 行为 |
| --- | --- | --- |
| `GET` | `/hub/status` | 返回 `{"running":true}`，用于检查服务响应。 |
| `GET` | `/hub/firmware/can/query/node/<id>` | 经 CAN 查询非当前运行的槽位，成功返回 `{"slot":"A"}` 或 `{"slot":"B"}`，查询超时为 5 s。 |
| `POST` | `/hub/firmware/can/send/node/<id>` | 请求体为 **Hub 本地镜像文件路径**；启动 CAN 下发，等待 Node 最终结果或失败后返回。 |
| `GET` | `/hub/firmware/tcp/query/node/<id>` | 通过节点已有 TCP 连接查询可升级槽位，成功返回 A/B；查询超时为 5 s。 |
| `POST` | `/hub/firmware/tcp/send/node/<id>` | 请求体为 **Hub 本地镜像文件路径**；通过已有 TCP 连接逐块下发，等待尾块校验结果或失败后返回。 |

以下以节点 3、树莓派 `192.168.1.112` 为例。

### 1. 查询目标槽位

```bash
curl http://192.168.1.112:8080/hub/firmware/can/query/node/3
```

若返回 `{"slot":"B"}`，选择 `edgenode/build/b/edegnode_image.bin`；返回 A 则选择 A 槽镜像。节点存在待确认升级或元数据无效时，接口返回错误。

### 2. 把对应镜像复制到 Hub

在编译完成后的 Windows 仓库根目录运行，下面的命令对应查询结果为 B：

```powershell
scp ./edgenode/build/b/edegnode_image.bin qxc@192.168.1.112:/home/qxc/Desktop/edegnode_b_image.bin
```

### 3. 发起下发

```bash
curl -i -X POST \
  http://192.168.1.112:8080/hub/firmware/can/send/node/3 \
  --data-raw '/home/qxc/Desktop/edegnode_b_image.bin'
```

`--data-raw` 发送路径字符串，Hub 按该路径打开文件；镜像内容通过上一步复制到树莓派。`-i` 显示 HTTP 状态码。Windows PowerShell 可使用 `curl.exe`，并将命令写为一行。

Hub 当前只允许一个固件发送任务，CAN 与 TCP 下发互斥。CAN 每帧携带 2 B 序号与 6 B 镜像数据，最多允许 8 帧未确认；收到 Node 的连续接收序号后推进窗口，500 ms 未获得进度时从未确认位置重传，连续最多重试 3 次。全部帧获进度确认后，最终结果等待时间为 5 s；有效的最终成功回复也可直接完成传输。

成功响应包含 `submitted`、`node`、`submitted_bytes` 和 `relay_verified`。`submitted=true` 表示整个文件已提交到对应 CAN/socket 发送接口；`relay_verified=true` 表示 Hub 收到 Node 的中转镜像校验成功回复。Node 此时提示手动重启，新 APP 的安装、启动和确认发生在用户复位之后，当前 HTTP 响应不报告这些阶段的结果。

文件不存在返回 404，传输忙或节点有待确认槽位返回 409，CAN/TCP 发送失败返回 503，确认超时返回 504。错误响应会提供 `error`；发送已经启动时还保留字节计数和校验结果，便于判断失败发生在哪个阶段。

### 使用 TCP 下发

文件准备和槽位选择与 CAN 相同，改用 TCP 路由；节点必须已连接到 Hub：

```bash
curl http://192.168.1.112:8080/hub/firmware/tcp/query/node/3

# 以下仍假设查询返回 B，且镜像已复制到 Hub
curl -i -X POST \
  http://192.168.1.112:8080/hub/firmware/tcp/send/node/3 \
  --data-raw '/home/qxc/Desktop/edegnode_b_image.bin'
```

TCP 每块最多 768 B 镜像数据，当前块成功确认后才发下一块；每次传输使用独立 `transfer_id`。整帧提交 socket 的超时为 5 s，普通块确认超时为 2 s，尾块含整包读回校验，确认超时为 10 s，每块最多重试 3 次。断线或 Node 要求从头开始时，本次下发结束，需要重新发起 HTTP 请求，不自动跨连接续传。

旧开发记录中的 `/hub/firmware/query/node/...` 与 `/hub/firmware/send/node/...` 属于历史路由，当前操作使用上表带 `/can/` 或 `/tcp/` 的路径。示例 IP、SSH 用户和文件路径按实际部署替换。

## 协议契约

| 链路 | 遥测 | ACK | 当前作用 |
| --- | --- | --- | --- |
| TCP | `EH + V5 + source + target + sequence + temperature + scale + CRC32`，固定 16 B。 | 同为 16 B，`source=0`、`target=nodeId`、保留温度字段为 0、成功状态为 0。 | 局域网主上报路径。 |
| CAN | 标准 ID `0x280 + nodeId`，DLC 7：`sequence + temperature + scale`。 | 标准 ID `0x300 + nodeId`，DLC 5：`sequence + status`。 | TCP 未确认时的回退路径。 |

TCP 与 CAN 都承载同一个 Message，却不强行共用同一份线上帧：TCP 需要魔数、版本和 CRC 来应对字节流重同步；CAN 控制器已有链路层 CRC，且 8 字节 DLC 限制要求紧凑的独立布局。

### CAN 固件协议

帧均为标准数据帧，以下多字节字段高字节在前，固件数据序号从 0 开始。

| 方向 | CAN ID | DLC | 载荷 |
| --- | --- | ---: | --- |
| Hub → Node | `0x380 + nodeId` | 8 | `sequence(2) + image_bytes(6)`，最后一帧不足部分填 `0xFF`。 |
| Hub → Node | `0x400 + nodeId` | 4 | `1 + request_id(2) + 0`，只查询可升级槽位。 |
| Node → Hub | `0x480 + nodeId` | 4 | `kind=1 + contiguous(2) + 0`，报告已连续收到第 N 帧。 |
| Node → Hub | `0x480 + nodeId` | 4 | `kind=2 + contiguous(2) + success`，`success=1` 表示中转区镜像校验成功。 |
| Node → Hub | `0x480 + nodeId` | 8 | `kind=3 + request_id(2) + status + target_slot(4)`，回复槽位地址。 |

槽位回复状态：0 为成功，1 为元数据无效，2 为存在待确认槽位；失败时目标地址为 0。成功地址为 A 槽 `0x08004000` 或 B 槽 `0x08021800`。例如 Node 3 的查询帧为 `0x403`，槽位、进度和结果回复都使用 `0x483`。

协议定义和实现见 [`can_frame.h`](../edgenode/User/App/Protocol/Can/can_frame.h)、[`can_output.c`](../edgenode/User/App/Output/Can/can_output.c) 与 [`OTA.cpp`](src/OTA.cpp)。

### TCP 固件协议

TCP OTA 与 16 B `EH` 遥测 ACK 共用连接，按类型和长度分流；多字节整数使用大端序，块号从 0 开始。

| 方向 | 类型 | 格式 | 长度 |
| --- | --- | --- | ---: |
| Hub → Node | 查询 `0x01` | `type(1) + target_node(1)` | 2 B |
| Node → Hub | 查询回复 `0x01` | `type(1) + slot(1)`；0 不可升级、1 为 A、2 为 B | 2 B |
| Hub → Node | 数据 `0x02` | `type(1) + transfer_id(4) + block(2) + length(2) + image_bytes(length)` | 10～777 B；9 B 帧头，有效载荷 1～768 B |
| Node → Hub | 数据回复 `0x02` | `type(1) + transfer_id(4) + block(2) + status(1)` | 8 B |

数据回复 `status=1` 为成功，块号对应已确认块；`status=2` 为失败，块号表示 Node 期待的块。尾块成功同时表示中转镜像完整校验通过。接收器按有效长度收齐数据，不能把一次 `+IPD` 当成一帧，也不能在固件载荷内部重新寻找 `type`。

定义与实现见 [`tcp_frame.h`](../edgenode/User/App/Protocol/Tcp/tcp_frame.h)、[`tcp_receive.c`](../edgenode/User/App/Protocol/Tcp/tcp_receive.c)、[`ota.c`](../edgenode/User/App/Service/Ota/ota.c) 和 [`OTA.cpp`](src/OTA.cpp)。

## 检查与排障

| 现象 | 先检查什么 |
| --- | --- |
| 启动报 CAN 初始化失败 | `ip -details link show can0`，确认接口存在、已配置并启动 |
| TCP / HTTP 监听失败 | `ss -lntp` 检查 8888/8080 是否被旧进程占用；检查是否误用 Ctrl+Z |
| 数据库打不开 | 启动工作目录、`data/` 是否存在、权限和磁盘空间 |
| HTTP 状态正常但查不到 Node | Node 的 Hub IP/端口、实际 TCP 连接、板号；CAN 查询则检查总线与对应节点 |
| 下发文件不存在 | POST 正文是 Hub 本地路径，不是 Windows 路径；先复制镜像，再发请求 |
| 下发返回 409 | 发送互斥、节点已有待确认/待安装镜像；核对是否需要先手动重启 |
| 下发返回 504 | 分开检查传输进度、节点写入/回复、连接断开及整包校验，超时不能直接等同于镜像已安装失败 |
| 收到 `relay_verified=true` 但版本没变化 | Node 还需手动复位；安装、试启动和确认不由这个 HTTP 响应报告 |

例如在 Hub 上查看本地服务，IP 与端口按实际运行替换：

```bash
curl http://127.0.0.1:8080/hub/status
ip -details link show can0
ss -lntp
```

如果安装了 sqlite3 CLI，可只读检查某条测试数据（替换板号和 sequence）：

```bash
sqlite3 data/edgehub.db 'SELECT nodeId, sequence, COUNT(*) FROM messages WHERE nodeId=3 AND sequence=123 GROUP BY nodeId, sequence;'
```

验证重传去重时，应同时保留 Node 补发/确认日志、Hub 收包/落库日志和数据库行数。只看 socket 发送成功或 HTTP 响应不足以证明整个遥测或升级链路。

## 验证状态与限制

- [2026-10-12 记录](../docs/develop/2026-10-12/main.md)包含 CAN A/B 升级、接收/搬运中断及坏镜像拒绝的实测。历史记录的无链路前缀路由不再用于当前操作，使用本文件的 `/can/` 和 `/tcp/` 路由。
- [2026-10-13 记录](../docs/develop/2026-10-13/main.md)包含 TCP OTA 的实现、编译与静态检查，TCP 真机下发、重复块确认、尾块校验、断线后重发仍待验收。
- 本次 README 整理没有重新部署 Hub 或运行设备测试；Node 主机回归及已知测试桩问题见 [EdgeNode README](../edgenode/README.md#主机回归与设备验证)。CMake 当前没有注册 Hub CTest 回归套件，编译成功不等于网络和故障验收通过。
- CAN 进度/结果帧没有传输编号，仍需考虑迟到旧回复；完成后的尾帧重传会补发成功结果，但接收中的重复/乱序帧不会推进序号，也不能假设每次重复帧都有新应答。
- TCP 数据帧带 `transfer_id` 并支持上一块重复应答；两字节槽位查询没有请求编号，Hub 在查询超时后关闭连接以隔离迟到回复。
- HTTP 管理未做身份认证，固件签名尚未实现；当前部署范围为受控局域网。
