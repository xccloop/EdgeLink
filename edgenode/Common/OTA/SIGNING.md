# OTA 签名

镜像头 v2 强制使用 ECDSA P-256/SHA-256。Hub下发前读取完整镜像验签；APP 接收后读回 GD25、Bootloader 搬运前读回 GD25、内部 A/B 槽启动前都验签，失败不进入对应成功路径。旧 v1 镜像被拒绝。

## 公共代码

- `Common/Crypto/micro-ecc/`：P-256 验签库，固定上游 commit，保留 BSD 许可证。
- `Common/Crypto/sha256/`：SHA-256，固定上游 commit，保留 public-domain 声明和本地移植修改记录。
- `Common/OTA/ota_signature.c/.h`：镜像摘要规则及分块验签接口，不依赖 FreeRTOS 或 Flash 驱动。
- `Common/OTA/ota_public_key.c/.h`：固件中的固定公钥；`ota_public_key.pem` 是主机构建/校验使用的同一公钥。

Node的APP和Bootloader分别编译链接Common中的公共源码。Hub是独立项目，验签库位于它自己的tools/crypto，镜像定义、验签接口和公钥位于tools/ota；不跨项目引用源码。Hub的CAN/TCP下发入口都执行完整镜像CRC和签名校验，两个项目必须使用一致的格式和公钥。设备不包含私钥，也不接受镜像自己提供的公钥。

## 镜像格式

镜像头仍为256字节，APP地址和A/B布局保持不变。

| 偏移 | 内容 |
| --- | --- |
| 0..27 | 原有magic、格式版本、大小、型号、固件版本、目标槽、APP长度及CRC |
| 28..31 | 小端算法编号1：ECDSA P-256/SHA-256 |
| 32..95 | 64字节签名，大端r[32]和s[32] |
| 96..251 | 保留字段，打包时填零，也受签名保护 |
| 252..255 | 小端header_crc32 |

签名输入为：**头前252字节（signature字段清零）+ 完整APP**。私钥签名后填入signature，最后计算包含实际signature的头CRC。CRC检查继续保留；它不承担来源认证。

## 本机编译

```powershell
python -m pip install -r ./edgenode/tools/requirements-signing.txt
./edgenode/tools/compiled.ps1 -v10
```

脚本默认使用 `%USERPROFILE%\.edgelink\keys\ota-signing.pem`，也可指定：

```powershell
./edgenode/tools/compiled.ps1 -v10 -SigningKey "D:\offline-keys\ota-signing.pem"
```

或设置 `EDGENODE_SIGNING_KEY` 环境变量。直接调用make时使用 `SIGNING_KEY` 参数。打包脚本检查私钥与仓库公钥配对，不匹配或缺少私钥则失败，不生成无签名镜像。输出仍为 `build/a/edegnode_image.bin` 和 `build/b/edegnode_image.bin`。

```powershell
mingw32-make -C edgenode/bootloader -j2
python ./edgenode/tools/verify_ota_image.py --input ./edgenode/build/a/edegnode_image.bin --slot a
```

## 密钥与首次迁移

本项目已生成一对密钥：私钥仅保存在本机仓库外，公钥已进入Common。私钥需要离线备份、限制访问；丢失后无法继续为当前信任根签名。不要把私钥传到Hub、Node或提交到Git。

全新信任根可以用 `tools/generate_ota_key.py --private-key <仓库外路径> --public-dir <公钥目录>` 生成。工具拒绝覆盖现有密钥文件。已有项目不要重新生成密钥来解决“缺少私钥”，应使用当前公钥对应的备份私钥。

**首次启用签名需要通过SWD配套部署新Bootloader和签名APP，并更新Hub。** 新Bootloader不启动旧无签名APP，旧Hub也会拒绝v2镜像。后续继续使用原来的CAN/TCP下发命令和手动重启流程。回退槽只有通过签名校验才可启动。

本次不增加防回滚版本计数、密钥轮换协议、固件加密或SWD物理防护；合法签名的旧固件仍可能被接受。

## 验证边界

```powershell
python ./edgenode/tools/test_ota_signature.py
python ./edgenode/tools/test_boot_signature.py
python ./edgenode/tools/test_ota_manual_restart.py
```

Hub在Linux或WSL中运行 `python3 edgehub/tools/test_ota_signature.py`，测试真实CAN/TCP下发入口，校验失败不会进入发送状态。它需要主机C/C++编译器和Python cryptography包。

测试使用独立测试密钥，覆盖Python签名/C验签、重新计算CRC后的篡改拒绝、错误密钥、旧格式、CAN/TCP接收结果、搬运前拒绝且不擦槽、内部槽拒绝。不会读取正式私钥。

Bootloader改为`-Os`保持16KiB分区；OtaTask栈增加到768 words（3KiB），并保留相关编译器栈使用报告。主机验证不能证明ARM汇编、板端最低栈水位、实际验签时间或签名升级端到端成功，这些需要烧录实测。
