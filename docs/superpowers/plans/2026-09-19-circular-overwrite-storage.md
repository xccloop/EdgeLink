# 循环覆盖 Flash 日志 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将当前“日志写满后拒绝新记录”的顺序追加日志改为按 4 KiB 扇区循环覆盖；网络短期或中期不可用时仍持续采集，容量耗尽时允许丢弃最旧扇区中尚未确认的数据。

**Architecture:** StorageTask 仍是 GD25Q32 的唯一读写者。Storage 在每次进入一个扇区前擦除该扇区，再写入 16 字节 pending 槽位；Flash 地址循环，`sequence` 永远递增。TransmitTask 只执行一次 TCP 优先、CAN 回退的投递尝试，并将成功或失败事件交回 Storage；Storage 持有唯一 in-flight 记录，失败后定时重投，期间继续把新采样落盘。

**Tech Stack:** GD32F103、GD25Q32 SPI NOR（4 KiB sector erase / 256 B page）、C11、FreeRTOS 静态队列与静态任务。

---

## 0. 已确认的业务语义与本期边界

本计划刻意采用**有界 best-effort**，不是无限缓存，也不是写满后仍严格 at-least-once。

```text
正常容量范围：首次完整写 pending -> Hub ACK -> 二次写 confirmed，仍是 at-least-once。

日志区绕回时：即将复用的 4 KiB 扇区先被擦除。
             其中若有 pending，最多 256 条最旧记录会丢失。

sequence：不随 Flash 地址回绕；Hub 的 (nodeId, sequence) 去重仍有效。
```

本期明确不实现：

- 擦除一个 4 KiB 扇区中途掉电后的 metadata journal 恢复；这是后续 metadata A/B 的独立工作。
- OTA 固件槽位、OTA Flag 格式或 Flash 地址分区修改。
- “返回所有 pending 地址”的 RAM 数组或大队列。
- 多条并发投递；同一时刻只有一条 Storage 认可的 in-flight 传输工作。

当前 16 字节槽位布局不变：

```text
[0..3]   sample_uptime_ms
[4..7]   sequence
[8..9]   temperature
[10]     temperature_scale
[11..14] CRC32，覆盖 [0..10]
[15]     delivery_status；0x00 = confirmed，其余 = pending
```

## 1. 文件与职责地图

| 文件 | 修改内容 | 不负责什么 |
|---|---|---|
| `edgenode/User/App/Output/Storage/storage.h` | 循环扫描、查询下一条 pending 的公开接口和返回语义 | 不管理 TCP/CAN、队列或任务重试时间 |
| `edgenode/User/App/Output/Storage/storage.c` | 全日志扫描、地址回绕、进入扇区前擦除、按地址顺序找下一条 pending | 不直接发送、不创建任务 |
| `edgenode/User/App/FreeRtos/Queue/rtos_queue.h/.c` | 增加 Transmit -> Storage 的失败事件队列；将传输工作队列限制为一条 | 不编码 ACK 帧、不访问 Flash |
| `edgenode/User/App/FreeRtos/Tasks/rtos_task.c` | Storage 的单条 in-flight 调度和重试计时；Transmit 的单次投递结果上报 | 不把 Flash 操作移到 TransmitTask |

不修改：`message.*`、TCP/CAN ACK 编解码、USART/CAN ISR、Hub、OTA 区域、GD25Q32 BSP 协议命令。

## 2. 运行时状态与事件

### 2.1 StorageTask 的状态

StorageTask 只需要以下 RAM 状态：

```c
typedef struct
{
    transmit_work_item_t work;
    TickType_t retry_due_tick;
    uint8_t active;
    uint8_t transmit_busy;
    uint8_t discarded_by_overwrite;
} storage_inflight_t;
```

- `active = 1`：这条 pending 仍由 Storage 调度，不能再次从扫描结果重复投递。
- `transmit_busy = 1`：该工作已交给 TransmitTask，等待成功或失败事件；此时不再向工作队列塞第二条。
- `discarded_by_overwrite = 1`：所在扇区已经按本期规则被擦除；收到这条旧工作的结果时只释放状态，不再确认或重试。
- `retry_due_tick`：TCP 与 CAN 都失败后，下一次允许重新投递该工作项的时刻。

### 2.2 两种 Transmit -> Storage 事件

保留既有成功确认事件，不改变其格式：

```c
typedef struct
{
    uint32_t sequence;
    uint32_t flash_address;
} storage_confirm_event_t;
```

新增失败事件，格式同样只定位工作项：

```c
typedef struct
{
    uint32_t sequence;
    uint32_t flash_address;
} transmit_failure_event_t;
```

失败事件的含义是“本次 TCP + CAN 尝试均未得到合法 ACK”，不是“这条数据永久发送失败”。Storage 收到它后继续采样，并在 1 秒后重投相同工作项。

### 2.3 完整数据流

```text
CollectTask -> collect_to_storage_queue -> StorageTask
                                             ↓ 首次 16B pending 写入
                                             ↓ 若没有 in-flight，提交最旧 pending
                                   storage_to_transmit_queue (长度 1)
                                             ↓
                                        TransmitTask
                                  TCP ACK 成功 ──────> confirm_queue
                                  TCP/CAN 均失败 ────> failure_queue
                                             ↓
                                        StorageTask
                                  confirmed -> 找下一 pending
                                  failed -> 1 秒后重试同一条
```

## 3. 实施任务

### Task 1: 固定循环覆盖的 Storage 公共契约

**Files:**

- Modify: `edgenode/User/App/Output/Storage/storage.h`
- Modify: `edgenode/User/App/Output/Storage/storage.c`
- Test: `edgenode` 全量构建

- [ ] **Step 1: 修改 `storage_init_result_t` 的注释和 `can_append` 语义。**

  `can_append` 不再表示“日志是否写满”，只表示 Storage 初始化成功且 `next_sequence` 没有到 `0xFFFFFFFF`。地址回绕不应令其变为 0。

  ```c
  /* 1 表示 sequence 仍可继续分配；日志末尾会循环到起点，不因地址满而关闭写入。 */
  uint8_t can_append;
  ```

- [ ] **Step 2: 在 `storage.h` 声明按环形物理顺序查找 pending 的接口。**

  ```c
  uint8_t storage_pending_find_after(
      uint32_t after_flash_address,
      telemetry_sample_struct *message,
      uint32_t *flash_address);

  uint8_t storage_next_write_erase_sector_get(uint32_t *sector_address);
  ```

  输入的 `after_flash_address` 必须是已经 confirmed 或已覆盖的槽位起始地址；函数从它的下一个槽位开始，环绕一次日志区，返回物理追加顺序中的下一条 CRC 有效且 status 非 `0x00` 的记录。找不到时返回 `STORAGE_FAIL`。

  `storage_next_write_erase_sector_get()` 不修改写指针：若下一次 `storage_write_pending()` 会先擦除一个扇区，则写入该扇区首地址并返回 `STORAGE_SUCCESS`；若下一次写入仍在当前扇区内，则返回 `STORAGE_FAIL`。StorageTask 只能通过此接口预先判断覆盖哪个 in-flight 地址，不能直接访问 `storage_next_address`。

- [ ] **Step 3: 增加 Storage 内部地址辅助函数。**

  在 `storage.c` 新增并使用：

  ```c
  /* gd25_clear() 的最小擦除单位；当前 BSP 未将该常量导出到 gd25.h。 */
  #define STORAGE_SECTOR_SIZE 4096U

  static uint32_t storage_address_next(uint32_t address)
  {
      address += STORAGE_SLOT_SIZE;
      return (address >= STORAGE_LOG_END_ADDRESS) ?
             STORAGE_LOG_BASE_ADDRESS : address;
  }

  static uint32_t storage_sector_base(uint32_t address)
  {
      return address - (address % STORAGE_SECTOR_SIZE);
  }
  ```

  当前 GD25Q32 BSP 将 4 KiB 常量保留在 `gd25.c`，未导出到 `gd25.h`，因此本期 Storage 保留同值宏并在注释中绑定 `gd25_clear()` 的擦除单位；不得在 APP 层调用 64 KiB 或整片擦除命令。

- [ ] **Step 4: 编译确认接口声明与现有调用未断裂。**

  Run:

  ```powershell
  Set-Location C:\Users\memory\Desktop\EdgeLink\edgenode
  & 'D:\Ksoftware\mingw64\bin\mingw32-make.exe' -B all
  ```

  Expected: exit code `0`，生成 `build/edegnode.elf` 与 `build/edegnode.bin`。

### Task 2: 将启动扫描改成完整环形扫描

**Files:**

- Modify: `edgenode/User/App/Output/Storage/storage.c`
- Test: `edgenode` 全量构建；Flash 手工构造验证

- [ ] **Step 1: 删除 `storage_scan_log()` 遇到第一个全 FF 槽位就 `return` 的行为。**

  循环日志可以有“新记录 -> 空槽 -> 旧记录”，因此必须读完
  `[STORAGE_LOG_BASE_ADDRESS, STORAGE_LOG_END_ADDRESS)` 的每一个 256 B 页面。

- [ ] **Step 2: 在完整扫描中同时记录以下信息。**

  ```c
  uint32_t max_sequence;
  uint32_t max_sequence_address;
  uint32_t oldest_pending_sequence;
  uint32_t oldest_pending_address;
  uint8_t has_valid_record;
  uint8_t has_pending;
  ```

  对每个槽位：

  - 全 FF：跳过，不可立即结束扫描；
  - CRC 错：跳过，不参与 sequence 或 pending；
  - CRC 对：比较 `[4..7]` sequence，更新最大 sequence 及地址；
  - CRC 对且 `[15] != 0x00`：比较最小 sequence，更新最旧 pending。

- [ ] **Step 3: 根据最大 sequence 恢复 `next_sequence` 和写指针。**

  没有有效记录时：

  ```c
  result->next_sequence = 0U;
  storage_next_address = STORAGE_LOG_BASE_ADDRESS;
  ```

  有有效记录时：

  ```c
  result->next_sequence = max_sequence + 1U;
  storage_next_address = storage_address_next(max_sequence_address);
  ```

  若 `max_sequence == 0xFFFFFFFFUL`，保持当前“不再分配新 sequence”的失败策略。

  当 `storage_next_address` 仍位于最大 sequence 同一扇区时，向后跳过非全 FF 槽位，避免把首次写入中断留下的半写槽位再次编程；到达扇区边界则保留边界地址，交给写入函数擦除下一个扇区。

- [ ] **Step 4: 手工验证启动恢复所需的四组 Flash 内容。**

  | 场景 | 期望结果 |
  |---|---|
  | 全 FF 日志区 | `next_sequence = 0`，写指针为 `0x000000`，无 pending |
  | 一次未回环的顺序记录 | 最大 sequence 后的空槽为写指针，最小 pending 被返回 |
  | 已回环：低地址是新 sequence、高地址是旧 sequence | 最大 sequence 地址后的槽位为写指针，不因中间空槽提前结束 |
  | CRC 错的半写槽位 | 不作为 pending，不复用该槽位，不让 sequence 倒退 |

- [ ] **Step 5: 编译并记录产物大小。**

  Run the Task 1 build command. Expected: exit code `0`，无新增 `-Wall -Wextra` 警告。

### Task 3: 在进入扇区前擦除并循环写入

**Files:**

- Modify: `edgenode/User/App/Output/Storage/storage.c`
- Test: `edgenode` 全量构建；单扇区循环写入硬件测试

- [ ] **Step 1: 在 `storage_write_pending()` 的写入前处理地址回绕。**

  ```c
  if(storage_next_address >= STORAGE_LOG_END_ADDRESS)
  {
      storage_next_address = STORAGE_LOG_BASE_ADDRESS;
  }
  ```

  删除原先“`storage_next_address + STORAGE_SLOT_SIZE > STORAGE_LOG_END_ADDRESS` 就返回失败”的满区退出分支。

- [ ] **Step 2: 仅在准备写入扇区首槽位时擦除整个 4 KiB 扇区。**

  ```c
  if((storage_next_address % STORAGE_SECTOR_SIZE) == 0U)
  {
      if(gd25_clear(storage_next_address) == 0U)
      {
          return STORAGE_FAIL;
      }
  }
  ```

  这发生在写入 `storage_next_address` 的 16 字节记录之前。一个扇区只擦一次，之后连续写满 256 条槽位；绝不能每写一条记录就擦除。


- [ ] **Step 3: 在擦除前让 StorageTask 得知即将覆盖的扇区。**

  `storage_write_pending()` 保持只处理 Flash，不直接访问任务状态。`storage_next_write_erase_sector_get()` 已在 Task 1 声明，由它返回即将擦除的扇区地址。新增一个仅由 StorageTask 调用的地址判断辅助函数：

  ```c
  uint8_t storage_address_in_sector(uint32_t flash_address,
                                    uint32_t sector_address);
  ```

  StorageTask 在调用 `storage_write_pending()` 前，先调用 `storage_next_write_erase_sector_get()`；若返回成功，再判断 `inflight.work.flash_address` 是否属于该扇区。若属于，将 `discarded_by_overwrite` 置 1。随后允许擦除继续写入。这正是本计划明确接受的“丢弃最旧 pending”时刻。

- [ ] **Step 4: 维持首次写入和确认写入的原子边界。**

  首次写入仍是完整 16 字节、status 为 `0xFF`；`storage_confirm()` 仍只写 `[15] = 0x00`，并读回精确 `0x00`。循环覆盖不得改变 CRC 覆盖范围或 confirmed 判定。

- [ ] **Step 5: 在目标板完成最小扇区循环验证。**

  为缩短测试时间，可仅在本地测试分支将日志终点临时定义为两个 4 KiB 扇区；不得把该临时宏提交到正式配置。

  验收：

  1. 写入超过 512 条记录后，扇区 0 被擦除并写入最新 sequence；
  2. 扇区 1 在进入前才被擦除；
  3. 写指针回绕，但 sequence 不回绕；
  4. 已覆盖扇区中的 pending 不再能由 `storage_confirm()` 确认。

### Task 4: 增加失败结果队列并限制工作队列为单条

**Files:**

- Modify: `edgenode/User/App/FreeRtos/Queue/rtos_queue.h`
- Modify: `edgenode/User/App/FreeRtos/Queue/rtos_queue.c`
- Test: `edgenode` 全量构建

- [ ] **Step 1: 将 Storage -> Transmit 工作队列长度改为 1。**

  ```c
  #define RTOS_STORAGE_TO_TRANSMIT_QUEUE_LENGTH 1U
  ```

  StorageTask 在 `transmit_busy = 1` 时不得发送第二条工作；队列长度 1 用来表达这个架构约束，而不是缓存历史地址列表。

- [ ] **Step 2: 定义失败事件和队列。**

  在 `rtos_queue.h` 增加：

  ```c
  #define RTOS_TRANSMIT_TO_STORAGE_FAILURE_QUEUE_LENGTH 1U

  typedef struct
  {
      uint32_t sequence;
      uint32_t flash_address;
  } transmit_failure_event_t;

  QueueHandle_t rtos_transmit_to_storage_failure_queue_get(void);
  ```

  在 `rtos_queue.c` 增加对应的 `StaticQueue_t`、静态 buffer、创建、NULL 校验和 getter。成功确认队列的名称、长度、事件格式都不变。

- [ ] **Step 3: 编译确认所有静态队列均创建成功。**

  Run the Task 1 build command. Expected: exit code `0`。

### Task 5: 让 StorageTask 调度单条 pending，同时持续写新采样

**Files:**

- Modify: `edgenode/User/App/FreeRtos/Tasks/rtos_task.c`
- Test: `edgenode` 全量构建；串口与 Flash 联调

- [ ] **Step 1: 在 `storage_task()` 添加 `storage_inflight_t`、失败队列句柄和重试常量。**

  ```c
  #define STORAGE_TRANSMIT_RETRY_MS 1000U
  ```

  初始化：

  ```c
  storage_inflight_t inflight = {0};
  failure_queue = rtos_transmit_to_storage_failure_queue_get();
  ```

  队列句柄为 NULL 时与现有队列一致，调用 `task_block_forever()`。

- [ ] **Step 2: 初始化成功后立即放行 CollectTask。**

  保持 sequence 只由 Message 管理的既有边界：

  ```c
  xQueueSend(sequence_queue,
             &storage_result.next_sequence,
             portMAX_DELAY);
  ```

  不再要求历史 pending 全部发送后才放行采样。

- [ ] **Step 3: 初始化时仅提交一条最旧 pending。**

  ```c
  if(storage_result.has_pending != 0U)
  {
      inflight.work.message = storage_result.oldest_pending_message;
      inflight.work.flash_address = storage_result.oldest_pending_address;
      inflight.active = 1U;
      inflight.transmit_busy = 1U;
      xQueueSend(transmit_queue, &inflight.work, portMAX_DELAY);
  }
  ```

- [ ] **Step 4: 优先处理确认和失败事件。**

  对成功事件：只有 sequence 和地址同时等于当前 `inflight.work` 时才改变调度状态。

  ```text
  active 且匹配且未被覆盖：调用 storage_confirm()
      成功 -> transmit_busy=0、active=0，按已确认地址查找下一 pending 并投递
      失败 -> transmit_busy=0，保持 active，retry_due_tick = 当前 tick + 1 秒

  active 且匹配且已被覆盖：不调用 storage_confirm()，transmit_busy=0、active=0，查找下一 pending

  不匹配：storage_confirm() 可自行拒绝；不得清除当前 in-flight。
  ```

  对失败事件：若它匹配一个已被覆盖的 in-flight，设置 `transmit_busy = 0U`、`active = 0U`，然后选择下一条仍存在的 pending；若它匹配当前且未被覆盖，则设置：

  ```c
  inflight.transmit_busy = 0U;
  inflight.retry_due_tick = xTaskGetTickCount() +
                            pdMS_TO_TICKS(STORAGE_TRANSMIT_RETRY_MS);
  ```

- [ ] **Step 5: 每轮处理一条新采样，但不因传输失败阻塞。**

  `collect_queue` 保持最多等待 100 ms。获得消息后：

  1. 调用 `storage_next_write_erase_sector_get()`；若返回扇区地址且该扇区包含 `inflight.work.flash_address`，将 `discarded_by_overwrite = 1U`；
  2. 调用 `storage_write_pending()`；
  3. 写成功且没有 `inflight.active` 时，将刚写入消息作为新的 in-flight 并投递；
  4. 写成功但已有 in-flight 时，只落盘，不追加工作队列；后续由顺序扫描补发；
  5. Flash 写入失败时保持现有 fail-closed 行为，调用 `task_block_forever()`。

- [ ] **Step 6: 在没有传输进行、且重试时间到达时提交当前 in-flight。**

  ```c
  if((inflight.active != 0U) &&
     (inflight.transmit_busy == 0U) &&
     (xTaskGetTickCount() >= inflight.retry_due_tick))
  {
      inflight.transmit_busy = 1U;
      xQueueSend(transmit_queue, &inflight.work, portMAX_DELAY);
  }
  ```

  `inflight.active == 0` 时，使用 `storage_pending_find_after()` 从刚刚 confirmed 或已覆盖的地址之后寻找下一条 pending；找到才投递，找不到则只继续服务 CollectTask。

- [ ] **Step 7: 编译并检查任务栈。**

  Run the Task 1 build command, then烧录。观察已有 `stack_monitor_task` 输出，确认 StorageTask 与 TransmitTask 的历史剩余栈没有接近 0。

### Task 6: 让 TransmitTask 只报告一次尝试的结果

**Files:**

- Modify: `edgenode/User/App/FreeRtos/Tasks/rtos_task.c`
- Test: `edgenode` 全量构建；TCP/CAN 失败与成功联调

- [ ] **Step 1: 取得失败事件队列句柄并检查。**

  ```c
  QueueHandle_t failure_queue;
  failure_queue = rtos_transmit_to_storage_failure_queue_get();
  ```

  与现有队列一致，获取失败后调用 `task_block_forever()`。

- [ ] **Step 2: 抽取一次投递函数，保持 TCP 优先、CAN 回退。**

  ```c
  static uint8_t transmit_work_try_once(
      const transmit_work_item_t *work,
      QueueHandle_t tcp_ack_queue,
      QueueHandle_t can_receive_queue)
  ```

  函数内顺序固定为：清 TCP ACK 队列 -> 发送 TCP -> 等 1 秒匹配 sequence 的 ACK；失败后清 CAN 队列 -> 发送 CAN -> 等 1 秒匹配 sequence 的 ACK。成功返回 1，其他情况返回 0。

- [ ] **Step 3: 成功只发送既有 confirm 事件，失败发送 failure 事件。**

  ```c
  if(transmit_work_try_once(&transmit_work,
                            tcp_ack_queue,
                            can_receive_queue) != 0U)
  {
      transmit_storage_confirm_send(confirm_queue, &transmit_work);
  }
  else
  {
      transmit_failure_event_t failure_event;
      failure_event.sequence = transmit_work.message.sequence;
      failure_event.flash_address = transmit_work.flash_address;
      (void)xQueueSend(failure_queue, &failure_event, portMAX_DELAY);
  }
  ```

  TransmitTask 不在本地无限重试同一个 work；失败后的重试节拍由 StorageTask 统一调度，这样 Storage 能在循环覆盖该扇区后取消已丢弃的工作。

- [ ] **Step 4: 验证 TransmitTask 不会阻塞采样。**

  断开 Hub 或让 Hub 不回复 ACK。观察：每次投递仍先等待 TCP、再等待 CAN；失败事件返回 Storage；CollectTask 仍每秒写入新 sequence，直到发生循环覆盖。

### Task 7: 端到端验收与提交边界

**Files:**

- Modify: `docs/develop/2026-09-19/main.md`
- Test: Node 编译、烧录、串口、Flash、Hub SQLite

- [ ] **Step 1: 运行源码与格式检查。**

  ```powershell
  Set-Location C:\Users\memory\Desktop\EdgeLink\edgenode
  & 'D:\Ksoftware\mingw64\bin\mingw32-make.exe' -B all
  Set-Location C:\Users\memory\Desktop\EdgeLink
  git diff --check
  ```

  Expected: 构建 exit code `0`，目标修改无 whitespace error。

- [ ] **Step 2: 验证正常 ACK 确认。**

  让 Hub 正常落库并回复 ACK。确认：一条 pending 被写入，ACK 的 sequence 匹配，状态字节读回 `0x00`，Hub SQLite 只存在一条 `(nodeId, sequence)`。

- [ ] **Step 3: 验证网络不可用时持续采集。**

  关闭 Hub 或阻断 ACK 至少 15 秒。确认：TransmitTask 每轮均执行 TCP 后 CAN，StorageTask 仍写入递增 sequence，CollectTask 和 LED 不停；不得因 `storage_to_transmit_queue` 满而阻塞 StorageTask。

- [ ] **Step 4: 验证扇区回绕覆盖。**

  使用临时两扇区日志范围完成测试后恢复正式宏。确认：进入旧扇区前先完成 `gd25_clear()`；新记录写入该扇区；旧扇区的 confirmed 与 pending 记录都不可再读取为有效待补传；新 sequence 连续递增。

- [ ] **Step 5: 验证被覆盖的 in-flight 不会卡住调度。**

  让最旧 pending 正在传输且无 ACK，制造写指针进入它所在扇区。确认：Storage 标记该工作已覆盖；之后即使收到该旧工作的失败/成功事件，均不写错误状态，也会释放调度并继续选择尚存记录。

- [ ] **Step 6: 在开发笔记中记录事实与边界。**

  记录构建命令、Flash 地址范围、实际 sequence、Hub SQLite 结果、是否覆盖了 pending；明确写出“本次未验证且不保证扇区擦除中掉电恢复”。

- [ ] **Step 7: 仅暂存本计划涉及的 Node 与开发笔记文件，再提交。**

  ```powershell
  Set-Location C:\Users\memory\Desktop\EdgeLink
  git add -- edgenode/User/App/Output/Storage/storage.h `
             edgenode/User/App/Output/Storage/storage.c `
             edgenode/User/App/FreeRtos/Queue/rtos_queue.h `
             edgenode/User/App/FreeRtos/Queue/rtos_queue.c `
             edgenode/User/App/FreeRtos/Tasks/rtos_task.c `
             docs/develop/2026-09-19/main.md
  git commit -m "(feat)storage日志区域循环写入"
  ```

  `docs/draw/`、`.vscode/`、无关 EdgeHub 文档不在本次暂存范围内。

## 4. 计划自检

- 覆盖范围：循环写入、完整扫描、单条 pending 调度、失败重试、覆盖 in-flight、构建与硬件验收均有任务。
- 不扩大范围：没有修改 TCP/CAN 帧格式、Hub、OTA、Flash BSP 命令或 metadata journal。
- 类型一致：成功事件继续使用 `storage_confirm_event_t`；失败事件单独使用 `transmit_failure_event_t`；两者均用 `sequence + flash_address` 标识工作项。
- 已知风险：擦除期间掉电、Flash 耗尽、扇区覆盖 pending 都不是隐含故障；前者明确延后，后两者作为本期业务策略与验收项保留。
