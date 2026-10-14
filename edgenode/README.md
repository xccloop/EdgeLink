# EdgeNode

EdgeLink 的 GD32F103RCT6 采集节点，使用 FreeRTOS 静态任务和队列。BMP280 温度先写入 GD25Q32 pending 日志，再经 ESP-AT TCP 上报，未确认时回退 CAN；收到 Hub 业务 ACK 后才确认 Flash 记录。ST7789 IPS 显示温度、链路、事件、积压和固件状态。

本文件负责节点配置、任务、分区、构建烧录、HMI 和验证。系统架构见[项目 README](../README.md)，HTTP 操作与线协议见 [EdgeHub README](../edgehub/README.md)，APP 职责见[分层说明](User/App/README.md)。

## 任务与目录

| 任务 / 模块 | 负责什么 | 不负责什么 |
| --- | --- | --- |
| CollectTask | 等待采集许可，生成 Message，交给 Log 与 HMI | 不发 TCP/CAN，不直接写日志 |
| LogTask | 日志初始化、pending 落盘、页索引、补发调度和 confirmed 写入 | 不直接执行 TCP/CAN 发送 |
| TransmitTask | 遥测 TCP 优先/CAN 回退、等待业务 ACK、执行链路恢复、发送 OTA 回复 | 不直接确认日志 Flash |
| OtaTask | 消费 CAN 帧/TCP 完整块，调用 OTA 服务并释放接收缓冲，管理空闲与启动确认 | 不在 ISR 中执行，不独立占用网络发送者 |
| HmiTask | 启动握手、构建视图快照、按键和 IPS 绘制 | 页面不自行读 Flash、ESP 或 BMP280 |
| LED / StackMonitor | 心跳指示与任务栈诊断 | 栈诊断不等于硬件看门狗或任务健康监督 |

入口为 [User/main.c](User/main.c)，依次完成初始化、静态队列与任务创建，然后启动调度器。APP 层组织和队列流见 [User/App/README.md](User/App/README.md)。

| 目录 | 用途 |
| --- | --- |
| [Drivers/BOARD/](Drivers/BOARD/) | 板级时基、共享 SPI 锁和事务边界 |
| [Drivers/BSP/](Drivers/BSP/) | BMP280、GD25、ESP、CAN、IPS 等器件访问 |
| [User/App/](User/App/README.md) | 模型、协议、输出、服务、FreeRTOS 与显示 |
| [Common/OTA/](Common/OTA/) | 镜像头、内部 Flash 分区与启动元数据 |
| [Common/ExFlash/](Common/ExFlash/) | 外置 Flash 统一分区 |
| [bootloader/](bootloader/) | 独立的启动、安装和回退工程 |
| [tools/](tools/) / [OCD/](OCD/) | 编译、打包、主机回归、烧录与调试配置 |

## 配置与构建

- 工具：Windows PowerShell、`mingw32-make`、ARM GNU Toolchain、Python 3。下载调试使用 ST-Link SWD 和 OpenOCD。
- 节点配置：[config.c](User/App/Config/config.c) 中设置 Wi-Fi、Hub 地址与端口、`board_id`。板号使用 1～127，同一 Hub 下不能重复。
- 驱动：使用 GD32F10x CMSIS 和标准外设库，不混用 STM32Cube HAL。BOARD 管理共享总线；BSP 管理具体器件。
- [Makefile](Makefile) 递归发现 APP/BSP `.c` 文件；新增标准外设库模块需加入 `DRIVER_SOURCES`。工具链绝对路径按本机调整。

在仓库根目录运行：

```powershell
# 版本号必填，示例设为 V7；仅编译，不连接设备或烧录
./edgenode/tools/compiled.ps1 -v7
```

| 布局 | 产物目录 | 用途 |
| --- | --- | --- |
| default / plain | `build/default/` | 独立 APP，从 `0x08000000` 启动 |
| A | `build/a/` | 带 256 B 镜像头的 A 槽应用，槽起点 `0x08004000` |
| B | `build/b/` | 带 256 B 镜像头的 B 槽应用，槽起点 `0x08021800` |

A/B 的 OTA 文件为 `edegnode_image.bin`，不能用未带头的普通 BIN 替代。版本号写入镜像头，构建规则也支持 `mingw32-make -C edgenode SLOT=all IMAGE_VERSION=7 "SIGNING_KEY=<仓库外私钥路径>"`。

## 固件签名

验签库及公共实现都放在 `Common`：`Common/Crypto` 保存SHA-256和micro-ecc，`Common/OTA`保存分块验签接口、镜像格式和固定公钥。APP接收读回、Bootloader搬运前和A/B启动前都验证签名，旧无签名镜像被拒绝。

首次构建安装 `python -m pip install -r ./edgenode/tools/requirements-signing.txt`；`compiled.ps1 -v10` 默认使用仓库外本机私钥，也可通过 `-SigningKey` 或环境变量指定。直接make需要传入 `SIGNING_KEY`。密钥备份、首次SWD迁移、验证命令和范围见[签名说明](Common/OTA/SIGNING.md)。

## 首次烧录与 OTA

```powershell
# 在已连接 ST-Link 的设备上首次部署 Bootloader 和 A 槽 APP
./edgenode/bootloader/flash.ps1
./edgenode/tools/flash.ps1 a
```

`flash.ps1 default` 写入 `0x08000000`，会覆盖 Bootloader；使用 A/B OTA 的设备应选择对应槽位。烧录脚本与 OpenOCD 配置见 [tools/flash.ps1](tools/flash.ps1)、[bootloader/](bootloader/) 和 [OCD/](OCD/)。

CAN 和 TCP 共用外部中转区及 Bootloader 安装流程。TCP 数据每块最多 768 B，写入成功后才应答，尾块需完成整包读回校验。接收完成后继续运行旧固件，IPS 提示手动重启；复位后安装，新 APP 运行约 10 秒后尝试确认启动。查询路由、镜像复制和下发命令统一见[EdgeHub 操作说明](../edgehub/README.md#http-管理与固件下发)。

## Flash 分区与记录格式

| 内部 Flash 区域 | 起始地址 | 容量 | 内容 |
| --- | --- | ---: | --- |
| Bootloader | `0x08000000` | 16 KiB | 启动、校验、安装与回退逻辑 |
| A 槽 | `0x08004000` | 118 KiB | 256 B 镜像头 + A 版 APP |
| B 槽 | `0x08021800` | 118 KiB | 256 B 镜像头 + B 版 APP |
| OTA 元数据 | `0x0803F000` | 4 KiB | 活动槽、待确认槽等启动状态 |

内部 Flash 分区定义见 [flash_layout.h](Common/OTA/flash_layout.h)。A/B 的向量表位于槽起点后 256 B，必须使用对应槽的链接布局和镜像，不能把默认 APP BIN 任意放到另一个地址。

| 外部 GD25Q32 区域 | 地址范围（末尾不包含） | 容量 |
| --- | --- | ---: |
| 遥测日志 | `[0x000000, 0x3BF000)` | 3836 KiB；15344 页 |
| OTA 中转 | `[0x3BF000, 0x400000)` | 260 KiB |

外部分区定义见 [external_flash_layout.h](Common/ExFlash/external_flash_layout.h)，GD25 一页 256 B，擦除扇区 4 KiB。

每条 Flash 记录固定为 16 字节：

```text
byte 0..3    sample_uptime_ms       节点采样时刻
byte 4..7    sequence               端到端业务身份
byte 8..9    temperature            int16，大端补码
byte 10      temperature_scale      int8，温度 = raw × 10^scale
byte 11..14  CRC32                  覆盖 byte 0..10
byte 15      status                 0x00 = confirmed；其他值 = pending
```

## 日志恢复

每条记录 16 B，日志区 `[0x000000, 0x3BF000)`，OTA 中转区 `[0x3BF000, 0x400000)`。进入新扇区前检查 pending，禁止擦除仍有有效待确认记录的扇区。

上电扫描恢复序号、写指针及页位图后放行 HMI 和可写时的采集；历史发送使用单步状态，不再等补完才放行。运行期重连捕获当时最大已落盘 sequence，每轮扫描最多 256 B，完整建立 1918 B 页位图后补发边界内的历史；扫描和等待历史 ACK 期间，新采样继续落盘并优先正常发送。

历史最多一条在途，普通在途工作处理完才投递历史，避免同一条记录的旧正常结果与历史结果混淆。新写入不让快照失效；同页追加/扇区擦除使受影响缓存重新读取，过期结果返回 `LOG_STALE`，不会确认复用后的新记录。单条历史连续失败三次退出；普通发送失败后，后续普通成功或重连会安排新一轮恢复。物理页顺序不保证回环后的 sequence 顺序，上电初始化扫描仍为同步全区扫描。

持续采集受实际容量约束：发送队列满时保留一个待投递工作并暂缓下次许可；目标扇区含 pending 时返回 `LOG_FULL`，确认释放空间后重新检查许可。普通发送持续占满链路吞吐时，历史补发可能长期等待；这些保护不会为了保持采样而覆盖未确认数据。

## 节点侧 HMI

节点带一块 320×240 的 ST7789 IPS 屏，由 `hmi_task` 独占驱动（1 ms 周期）。它**不直接读**传感器、Flash 或 ESP——其他任务把数据填进一份完整快照（`hmi_view_data_t`），HMI 只负责显示。

| 页面 | 显示什么 | 数据来源 |
| --- | --- | --- |
| **HOME** | 温度、Wi-Fi 名字与本机 IP | `collect_to_hmi_queue`；`tcp_ssid_get()` / `tcp_local_ip_get()` |
| **LINKS** | TCP / CAN 最近一次通信结果 | `tcp_connected_get()`；`can_state` |
| **EVENTS** | 最近 5 次收发的滚动窗口（终端式，新的从下面进） | `transmit_to_hmi_log_queue`——TransmitTask 每发完一条交一行 |
| **BACKLOG** | Flash 里还没被 Hub 确认的 pending 条数 | `log_pending_count_get()` |

底部 28 像素是导航条：**KEY1/KEY2** 移动候选页，**KEY3** 确认切换。按键走 EXTI 中断 + 位图事件，由 HMI 任务取走并做 50 ms 去抖。

刷新不是整屏双缓冲：`hmi_service()` 每次只合成**一行**，212 行的正文要约 212 ms 画完，所以物理屏幕上能看到逐行刷新。绘制期间的数据更新不会打断当前场景——`hmi_set_view_data()` 把待刷新行范围合并起来，等空闲时再一起提交。

HMI 首帧启动页完成后才通知 LogTask 开始恢复，存储前置流程结束后再进入正常页面。版本号由 OTA 服务读取元数据 `active_slot` 对应的镜像头；新固件试启动尚未确认时，可能仍显示旧的已确认版本。中转镜像校验完成后显示 `New Version please Restart`，等待用户手动重启。

四个页面都无法自行探测或推断状态：链路状态只反映「调用方最近一次观测」，`UNKNOWN` 表示「还没有结果」，和 `OFFLINE`（确认失败）不是一回事。**BACKLOG 页用于观察本地积压**——只有匹配 Hub ACK 并成功确认 Flash 后计数才下降，扫描建图期间不会因“找到记录”而减少。

分层见 [`Presentation/Hmi/`](User/App/Presentation/Hmi/)：

```text
Widget  →  Page   →  Render  →  Control
点阵字体    页面坐标    逐行合成    持有快照、当前页、
矩形/文本   配色       交 DMA      待刷新范围
```

## 主机回归与设备验证

在仓库根目录运行（主机 C 编译器默认 `gcc`，可通过 `HOST_CC` 指定）：

```powershell
python edgenode/tools/test_log_replay.py
python edgenode/tools/test_tcp_recovery.py
python edgenode/tools/test_ota_manual_restart.py
```

以上三项在 2026-10-14 的文档核验中通过。旧的 `test_esp_diagnostics.py` 尚未适配新版 TCP 接收代码，当前因缺少 `task.h` 测试桩而编译失败，需要单独修复。

`test_log_replay.py` 模拟 Flash/队列并运行真实 Log/LogTask；`test_tcp_recovery.py` 模拟 UART 与时间；`test_ota_manual_restart.py` 模拟中转区并运行真实 OTA/HMI Page。三者都需要主机 C 编译器，但不模拟完整硬件实时性。[10 月 12 日记录](../docs/develop/2026-10-12/main.md)包含 CAN 升级和部分故障实测；[10 月 13 日记录](../docs/develop/2026-10-13/main.md)包含 TCP OTA 和页索引实现。新版扫描的 IPS 刷新、实际耗时以及 TCP OTA 端到端仍需设备验收，v2 固件签名已实现并通过主机验证，签名版实机验收仍待完成。
