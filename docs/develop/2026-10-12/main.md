# 2026-10-12

## 1.昨天解决了TCP冻结，今天继续测试OTA部分

但是我发现现在不是很好的可以测试，我们在IPS上打印一下现在的版本号方便我们去看
现在ota.c中添加获取版本号也就是读取镜像头

```c
/* 版本来源固定为元数据 active_slot，不读取待安装的中转镜像。 */
uint8_t ota_active_version_get(uint32_t *version)
{
    ota_metadata_t meta;
    const ota_image_header_t *header;

    if(version == NULL)
    {
        return 0U;
    }
    if(ota_metadata_load(&meta) != OTA_METADATA_LOAD_REAL)
    {
        return 0U;
    }
    if((meta.active_slot != OTA_SLOT_A_BASE_ADDRESS) &&
       (meta.active_slot != OTA_SLOT_B_BASE_ADDRESS))
    {
        return 0U;
    }

    header = (const ota_image_header_t *)meta.active_slot;
    if((header->magic != OTA_IMAGE_MAGIC) ||
       (header->header_version != OTA_IMAGE_FORMAT_VERSION) ||
       (header->header_size != OTA_IMAGE_HEADER_SIZE) ||
       (header->hardware_model != OTA_CURRENT_HARDWARE_MODEL) ||
       (header->target_slot != meta.active_slot) ||
       (crc32_check((const uint8_t *)header, OTA_IMAGE_HEADER_CRC_LENGTH,
                    header->header_crc32) == 0U))
    {
        return 0U;
    }

    *version = header->firmware_version;
    return 1U;
}
```

然后后续给HMI调用

```c
        if((xTaskGetTickCount() - version_check_tick) >= pdMS_TO_TICKS(1000U))
        {
            uint32_t version = 0U;
            uint8_t version_valid = ota_active_version_get(&version);

            version_check_tick = xTaskGetTickCount();
            if((version_valid != hmi_snapshot.firmware_version_valid) ||
               (version != hmi_snapshot.firmware_version))
            {
                hmi_snapshot.firmware_version = version;
                hmi_snapshot.firmware_version_valid = version_valid;
                need_commit = 1U;
            }
        }
```

    
我们先修改
compiled.ps1文件第四十一行

```powershell
& $make_path -B SLOT=all IMAGE_VERSION=2 all
```

这样打包后的版本就是就是V2方便后续的测试
随后通过wget查询目标节点槽位

```text
[root@buildroot]:~$:wget -qO- http://192.168.1.4:8080/hub/firmware/query/node/3
{"slot":"B"}
```

接下来通过scp将固件传输到LS2K0300上

```powershell
 scp "C:\Users\memory\Desktop\EdgeLink\edgenode\build\b\edegnode_image.bin" root@192.168.1.4:/home/root/firmware/node_b_v2.bin
```

然后通过如下命令进行指定节点的指定固件下发

```text
[root@buildroot]:~$:wget -qO- \
>   --header="Content-Type: text/plain" \
>   --post-data="/home/root/firmware/node_b_v2.bin" \
>   http://192.168.1.4:8080/hub/firmware/send/node/3
{"submitted":true,"node":3,"submitted_bytes":45024,"relay_verified":true}
```

最后返回true说明固件传输成功，固件传输成功后，node进行复位重启，随后IPS显示从V1变为V2
说明此时固件传输成功。
以下是本次测试遇到的一个问题主要是IPS，在运行过程中复位，IPS不会复位导致会显示上一轮的IPS显示

我想到一个修复方法，现在的花屏和保留上一轮页面本质是一个问题，由于LOG任务先运行，导致HMI任务后运行，那么只有等LOG的前置流程完毕后再给HMI运行的机会
我们可以这样做，HMI新增一个启动页面，我们先把LOG进行阻塞，由于HMI优先级比LOG低一位，那么此时HMI开始工作，我们让他先画一个启动页面
然后告诉LOG你可以开始工作了，并且把HMI设为阻塞，那么等LOG一切运行完毕之后，在告诉HMI你可以开始工作了这样就不会有出现花屏和保留上一轮的情况
并且这样做也更像加载画面了

还有一个问题：当前 Node 接收完固件并校验通过后，ota_task 会自动重启。我认为应当把重启时机交给用户决定。
修复方法为删除 ota_task 中固件接收完成后调用的 NVIC_SystemReset()，改为：
固件接收并校验通过后，Node 继续运行当前固件，HMI 在当前版本号旁显示小字 New Version please Restart，提示用户有新固件等待安装。
等待期间保留已校验的固件，不因接收超时清除完成状态，也不允许后续传输覆盖它。
用户按下复位键或重新上电后，Bootloader 才安装并启动新固件。Hub 保持现有成功返回格式，无需参与用户重启和后续安装流程。

感觉差不多了，基本上就是IPS的问题，正常传输OTA没有问题并且之前测试交替固件下发也没有问题

接下来我们进行故障测试，这是重中之重，这个通过以后意味着后续增加TCP的OTA也会简单很多

## 2.下发固件传输一半断联（包括node关闭或者是CAN损坏）

预测情况，新固件不会正常从外置flash中加载，版本号保留在旧固件
在输入下发命令后直接给node断电然后重新上电
发现版本号保持V4没有新固件提示并且再次查询槽位保持不变
这是LS2K0300的终端

```text
[root@buildroot]:~$:wget -qO- http://192.168.1.4:8080/hub/firmware/query/node/3
{"slot":"A"}
[root@buildroot]:~$:wget -qO- --header="Content-Type: text/plain" --post-data="/home/root/firmware/node_a_v5.bin" http:/
/192.168.1.4:8080/hub/firmware/send/node/3
wget: server returned error: HTTP/1.1 504 Gateway Timeout
[root@buildroot]:~$:wget -qO- http://192.168.1.4:8080/hub/firmware/query/node/3
{"slot":"A"}
```

返回504为正常现象，符合hub收不到node回复

## 3.下发新固件后，bootloader获取固件，在获取一半的时候进行掉电

预测情况，设备能够恢复安装或启动有效旧固件，不因安装中断而无法启动。
实测：再次上电后，Bootloader 重新尝试搬运固件，最终成功启动新版本。本次掉电恢复测试通过，设备未变砖。
测试通过

## 4.中断后重新下发

预测：固件	接收校验成功，手动复位后启动新版本
2，3用的测试固件为同一套，所以此条算通过

## 5.新版本确认与 A/B 双向升级

预测，查询槽位每次升级成功后翻转并且版本号变更
这个是前面实验的基础，通过

## 6损坏固件拒绝，修改固件应用区一个字节，保留原 CRC 后下发。

预期校验失败、不提示重启；复位仍运行旧版本，之后正常固件还能升级。
V6下发

```text
[root@buildroot]:~$:wget -qO- --header="Content-Type: text/plain" --post-data="/home/root/firmware/node_b_v6.bin" http:/
/192.168.1.4:8080/hub/firmware/send/node/3
{"submitted":true,"node":3,"submitted_bytes":46136,"relay_verified":true}
```

此时板卡显示新固件已到来，不符合预期，继续测试，此时重启看一下是否会加载错误固件V6
错误固件V6正常加载，不对啊，原来是下发错了，把正确的固件下发了
再次下发错误固件V7

```text
[root@buildroot]:~$:wget -qO- \
>   --header="Content-Type: text/plain" \
>   --post-data="/home/root/firmware/node_a_v7_bad_crc.bin" \
>   http://192.168.1.4:8080/hub/firmware/send/node/3
wget: server returned error: HTTP/1.1 504 Gateway Timeout
```

此时显示504符合预期
Node 收完整份固件，应用 CRC 校验失败。
Node 清除接收状态，没有发送失败结果。
Hub 等不到最终结果，返回 504 Gateway Timeout。
IPS未显示有新固件到来，现在重启验证是否加载错误固件V7
板卡正常启动V6，错误固件V7未加载，错误固件通过

现在所有测试通过，判断OTA满足掉电安全
对了重连的扫描整个flash还没解决

接下来我们做TCP链路的OTA升级，现有OTA其实挺完善的，不需要大改，主要是把他从can专门适配改为TCP适配
并且加入tcp的ota分流

第一步我们的想法是确定 TCP 帧格式，然后进行分流。先做 query，也就是查询帧，考虑如下：
| 方向 | 格式 | 长度 |
| --- | --- | --- |
| Hub → Node | type / target_node | 2 字节 |
| Node → Hub | type / slot | 2 字节 |
其中 type 为 0x01，表示查询；slot 为 1 表示 A 槽、2 表示 B 槽、0 表示当前不可升级。这里查询的是下一次升级应该下发的槽位。
我们首先需要修改 ESP12S 的接收逻辑，因为原来只保留长度为 16 字节的数据，两字节的查询帧会被丢弃。修改后，底层按照 +IPD 中的长度接收负载，通过回调交给 APP。底层不判断业务类型，也不直接调用业务队列。
接下来在 APP 层进行组帧和分流。收到消息开头后，如果是 0x01，就按两字节查询帧接收；如果是 EH，就按原来的 16 字节 ACK 接收。收完整后，查询进入 OTA 队列，ACK 进入原有 ACK 队列。
这里不能把一次 +IPD 当成一帧，因为 TCP 可能拆包或者粘包。因此没有收完整时保留组帧进度，收完整一帧后再判断后面的消息。
随后 OtaTask 从队列取出消息，通过来源标记区分 CAN 和 TCP。CAN 交给 ota_can_on_frame，TCP 交给 ota_tcp_on_frame。TCP 查询先检查目标节点是否为本机，再读取 OTA 状态，确定下一次可升级的槽位；正在接收固件、等待安装或者元数据无效时，返回不可升级。
槽位判断完成后，OtaTask 将结果交给回复队列，由 TransmitTask 封装为 type / slot，通过原来的 TCP 连接回复。这样仍然只有 TransmitTask 操作发送，不会与遥测发送同时操作 ESP。
Hub 侧收到 HTTP 查询后，先找到目标节点对应的 TCP 连接，封装并发送查询帧，同时记录这个查询对应哪个 HTTP 请求。Node 回复后，Hub 在 TCP 接收处将查询回复与遥测分流，再根据槽位生成 JSON，返回给等待中的 HTTP 客户端。
这样一次查询就完成了：HTTP 发起查询，Hub 通过 TCP 询问 Node，Node 判断槽位并回复，Hub 最后将结果返回 HTTP。
最终结果在images下，我们顺便修改了can链路的查询命令

有点复杂了，下次再看看吧
