# EdgeHub V5 ACK 设计

## 目标

EdgeHub 接收 Edgenode V5 遥测后，以 `(nodeId, sequence)` 在 SQLite 去重；新记录与已存在记录均回复成功 ACK，SQLite 真实错误不回复。

## 边界

- TCP 遥测与 ACK 均为 16 字节 V5 帧。
- CAN 遥测使用 `0x280 + nodeId`、DLC 7；CAN ACK 使用 `0x300 + nodeId`、DLC 5。
- `sequence` 为 `uint32_t`。
- 不增加节点任务、不实现节点接收 ACK 的运行时路径、不迁移旧 SQLite 表。

## 数据流

接收端先完成帧校验，再持久化。`Inserted` 与 `Duplicate` 均生成 `ack_status=0`；`Error` 仅记录错误。开发数据库由人工清理后以带唯一索引的表创建；旧库如已有重复 `(nodeId, sequence)`，启动会拒绝创建索引，必须先手动删除 `edgehub/data/edgehub.db`，代码不会自动清库。
