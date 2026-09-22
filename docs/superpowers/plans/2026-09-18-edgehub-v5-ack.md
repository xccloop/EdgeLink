# EdgeHub V5 ACK Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 EdgeHub 按 Edgenode V5 协议接收遥测、SQLite 去重并在成功持久化或重复命中后回复 TCP/CAN ACK。

**Architecture:** `TcpFrame` 负责固定 16 字节 V5 解析和 ACK 编码；`Storage` 返回持久化结果；`Gateruntime` 连接接收、持久化结果与对应输出 ACK。现有 `Can`、`TcpConnection` 发送出口复用。

**Tech Stack:** C++17、Linux SocketCAN、TCP socket、SQLite3、CMake。

---

### Task 1: V5 数据模型和 TCP 帧

**Files:**
- Modify: `edgehub/inc/Message.hpp`
- Modify: `edgehub/inc/TcpFrame.hpp`
- Modify: `edgehub/src/TcpFrame.cpp`

- [ ] 将 `sequence` 统一为 `uint32_t`，将 V5 遥测解析为 `EH|05|source|0|sequence(4)|temperature(2)|scale|crc32`。
- [ ] 增加 `tcp_ack_encode(frame, node_id, sequence, status)`，编码 `EH|05|0|node_id|sequence(4)|00|00|status|crc32`。

### Task 2: SQLite 去重结果

**Files:**
- Modify: `edgehub/inc/Storage.hpp`
- Modify: `edgehub/src/Storage.cpp`

- [ ] 创建表时定义 `UNIQUE(nodeId, sequence)`。
- [ ] 使用 `ON CONFLICT(nodeId, sequence) DO NOTHING`，并以 `sqlite3_changes()` 返回 `Inserted` 或 `Duplicate`；其他约束、prepare、bind、step 失败均返回 `Error`。

### Task 3: TCP/CAN ACK 调度

**Files:**
- Modify: `edgehub/inc/Gateruntime.hpp`
- Modify: `edgehub/src/Gateruntime.cpp`

- [ ] TCP 持久化结果为 `Inserted` 或 `Duplicate` 时，向来源连接发送 V5 ACK。
- [ ] CAN 使用 V5 的 `0x280 + nodeId`/DLC7 解码，并在成功持久化或重复命中后发送 `0x300 + nodeId`/DLC5 ACK。

### Task 4: 验证

**Files:**
- Modify: 上述文件

- [ ] 执行 `cmake -S edgehub -B edgehub/build` 与 `cmake --build edgehub/build`。
- [ ] 检查 `git diff --check`，人工核对 ACK 仅发生在 `Inserted` 或 `Duplicate` 分支。
