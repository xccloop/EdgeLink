# APP 分层

这里不是按外设名称分类，而是按一条遥测数据从产生到输出的职责分类：

```text
Acquisition -> Model -> Service/node_service -> Output/Storage（当前已接入）
                                            -> Service/Transmit -> Protocol/Tcp -> Output/Tcp（待接入）
                                                                 -> Protocol/Can -> Output/Can（待接入）
                                            -> Presentation/Hmi（后续实现）
```

- `Config`：板号、网络地址、采样周期等稳定策略；不存放运行时状态。
- `Startup`：唯一的初始化编排入口；不包含主循环业务。
- `Acquisition`：读取 BMP280 等外设，产出原始采样结果。
- `Model`：统一的 `telemetry_sample_struct`；它不知道数据将被怎样输出。
- `Protocol`：TCP/CAN 帧字段和 CRC；只负责编码与校验，不直接操作硬件。
- `Output`：每个外部出口的副作用边界。Storage 写 Flash，Tcp 管理 ESP-AT/TCP，Can 调用 CAN BSP。
- `Service`：决定一次采样要走哪些输出分支；当前 `Transmit` 仍处于逐步实现阶段。
- `Presentation`：显示和交互；`Buffer` 是 IPS DMA 的内部支撑，后续 `Hmi` 放在这里。

`main()` 只负责启动并周期性调用 `node_service_run_once()`。它不应直接再初始化 Storage，也不应了解帧字节布局。
