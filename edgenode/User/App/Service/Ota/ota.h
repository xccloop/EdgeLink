#ifndef OTA_H_
#define OTA_H_
#include <stdint.h>

/*
    一帧 OTA 数据帧的形状
*/
#define OTA_FRAME_LENGTH    8U
#define OTA_FRAME_PAYLOAD   6U

/*
    攒够多少字节刷一次外部 Flash：
    flash一页是256，但是can payloade一帧才6，单纯放除不尽干脆攒够再放
*/
#define OTA_BLOCK_SIZE      768U

/*
    ota_on_frame / ota_on_idle 的返回值。
    这是 ota.c 唯一的"对外说话方式"—— 它不发帧、不碰队列，
    只把这个结构体填好，交给外壳去办。

    reply       1 = 建议立刻回复
    contiguous  回复内容：我连续收到第 N 号
    finished    1 = 整个流程结束
    success     finished 时为 1 表示成功
*/
typedef struct
{
    uint8_t reply;
    uint16_t contiguous;
    uint8_t finished;
    uint8_t success;
} ota_result_t;

/*
    外壳每 OTA_IDLE_REPLY_MS 毫秒调一次 ota_on_idle；
    ota.c 靠数这个次数来推断「这次传输已经死了」，累计到 OTA_TRANSFER_TIMEOUT_MS 就清状态。
    （ota.c 不认识 FreeRTOS，拿不到时钟，所以只能靠「被叫了多少次」。）
*/
#define OTA_IDLE_REPLY_MS         3U
#define OTA_TRANSFER_TIMEOUT_MS   5000U

/*
    回复的节流。两个触发点，缺一个都会出问题：

    ① 每收满 OTA_REPLY_EVERY_FRAMES 帧报一次 —— 正常推进时的进度。
       一个窗口 8 帧，正好对上，Hub 每发完一个窗口就能收到一次。
    ② 闲着 OTA_IDLE_REPLY_AFTER_MS 还没新帧，再报一次（只报一次，不刷屏）。
       【没有它会死锁】：断号时 Node 在等一个永远不来的帧，永远攒不满 K 帧
       → 永不回复 → Hub 永远在等。

    OTA_REPLY_POLL_MS：TransmitTask 等遥测队列的超时。它原来是 portMAX_DELAY，
    但那样就永远醒不过来取 OTA 回复了（遥测 1 秒才一条，回复延迟可能到 1 秒）。
    这个值同时决定回复的最大延迟。
*/
#define OTA_REPLY_EVERY_FRAMES    8U
#define OTA_IDLE_REPLY_AFTER_MS   20U
#define OTA_REPLY_POLL_MS         5U

/*
    应用启动后跑满这么久，才向 bootloader 销账（宣布"我这次真的跑起来了"）。

    为什么不能一上电就销账：那等于把安全网拆了。
    新固件如果"启动后 200ms 就崩"，销账已经发生 → bootloader 不再数试启动次数
    → 永远退不回旧槽，只能拆机重烧。

    10 秒：短到用户无感，长到足够暴露"一启动就崩"这种毛病。
*/
#define OTA_CONFIRM_DELAY_MS      10000U

/*
    收满 + 校验通过之后，等结果回复真的发出去，再复位整机。

    第一步等"回复出队"：TransmitTask 每 OTA_REPLY_POLL_MS 来取一次，取走就发。
    第二步再留一点余量让 CAN 帧离开总线 —— 复位会掐断还没发完的帧。

    两个都是【上限】，等不到也照样复位：复位才是升级的目的，
    不能因为 Hub 没收到就停在这儿。最坏 150ms。
*/
#define OTA_RESET_FLUSH_WAIT_MS   100U
#define OTA_RESET_FLUSH_MS        50U

/* 1. 吃一帧 */
ota_result_t ota_on_frame(uint16_t standard_id, const uint8_t *data, uint8_t data_length);

/* 2. 闲着的时候问一句（外壳在队列超时时调） */
ota_result_t ota_on_idle(void);

/* 3. 把状态清回初始  */
void ota_reset(void);

/*
    4. 销账：宣布"我这次真的跑起来了"。

    由应用自己做，因为 bootloader 做不了 —— 跳转之后它就是上一个生命周期的人了，
    看不见"应用跑起来了"还是"刚跳过去就复位了"。只有应用自己知道。

    少了这一步：bootloader 会一直数试启动次数，数到 3 就判新固件起不来，
    把设备退回旧槽 —— 哪怕新固件跑得好好的。

    幂等且很便宜：已经销过账时只读一次元数据就返回，不写 Flash。
    返回 1 = 已经确认（本来就是，或者刚写成功）。
*/
uint8_t ota_confirm_boot(void);

#endif