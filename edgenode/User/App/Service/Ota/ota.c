/*
    这个文件是用于处理固件的，我们已经从can接受中断中获取到了ota固件一帧，但是接下来很有事情需要做
    ota_receive 要干的活，分三段
    【逐帧处理】—— 每收到一帧都跑
    ① 节点号过滤    不是发给我的 → 丢（安全要求，CAN 是广播的）
    ② 序号状态机    和 expected_seq 不对 → 丢（这就是"回退 N"的接收侧）
    ③ 攒缓冲        6 字节 memcpy 进 ota_block
    ④ 满 768 → 刷   切成 3 次 256 写进中转区
    ⑤ 收满 256 时   解析镜像头，取出 total 和 crc（只做一次）
    【收尾】—— 收满总长时跑一次
    ⑥ 刷最后一块    那个"不满 768"的坑
    ⑦ 读回算 CRC    数据在 flash 里，要读回来验
    ⑧ 通过 → 写"待装"标志
    ⑨ 任何一步失败 → 清理状态，退回 IDLE（绝不能卡死）
    【对外】
    ⑩ 该回复进度时，发「我连续收到第 N 号」

    本文件不认识 FreeRTOS、不认识队列、不调用任何发送函数。
    它只做决策：吃进一帧或一次"我闲着了"，吐出一个 ota_result_t。
*/

#include "ota.h"
#include "Protocol/Can/can_frame.h"
#include "Config/config.h"
#include "GD25Q32/gd25.h"
#include "ExFlash/external_flash_layout.h"
#include "OTA/ota_image.h"
#include "CRC/crc32.h"
#include <stdint.h>
#include <string.h>

/*
    中转区内部的布局（见设计文档）：
        OTA_RELAY_BASE_ADDRESS + 0       头部 1 个扇区：「待装」标志
        OTA_RELAY_BASE_ADDRESS + 4096    镜像本体        ← 本文件只碰这里

    开头那个扇区是 Task 8（待装标志）的事，本文件一个字都不写它。
    少加这 4096，写镜像的第一步就会把待装标志覆盖掉。
*/
#define OTA_RELAY_IMAGE_BASE  (OTA_RELAY_BASE_ADDRESS + GD25Q32_SECTOR_SIZE)

/* 镜像本体最大能有多大；超过就放弃，绝不让写入地址跑出中转区踩到日志区。 */
#define OTA_RELAY_IMAGE_CAPACITY  (OTA_RELAY_END_ADDRESS - OTA_RELAY_IMAGE_BASE)

/*
    跨帧状态：这些值必须从第一帧活到最后一帧，所以住在文件级。

    注意 block_offset / erased_bytes 存的是【相对镜像区起点的偏移】，不是绝对地址。
    这样「全 0」天然就是一个合法的初始状态 —— C 语言白送。
    若存绝对地址，初始值会是 0x3C0000，就得在「文件级定义」和「复位函数」两处各写一遍，
    迟早写漏一处，而且是那种跑很久才炸的漏法。
*/
typedef struct
{
    uint16_t expected_seq;    /* 我期待的下一个序号 */
    uint16_t block_bytes;     /* 当前块已经攒了多少字节 */
    uint32_t block_offset;    /* 当前块要写到镜像区内的哪个偏移 */
    uint32_t received_bytes;  /* 已经收了多少字节（含 256 字节镜像头） */
    uint32_t written_bytes;   /* 已经真正写进中转区多少字节 */
    uint32_t erased_bytes;    /* 镜像区内已经擦成全 1 的连续字节数 */
    uint32_t image_total;     /* 镜像总长；0 = 还没解析到镜像头 */
    uint32_t image_crc32;     /* 镜像头里的 application_crc32 */
    uint32_t idle_ms;         /* 已经闲着多久（毫秒），用来推断传输死没死 */
    uint8_t  done;            /* 1 = 收完且校验通过；后面再来的帧一律不理 */
} ota_rx_state_t;

static ota_rx_state_t ota_state;

/*
    攒帧缓冲：768 字节。
    放文件级，不进任何任务的栈 —— 任务栈只有 512 字（2048 字节），
    这一个数组就能吃掉 37.5%，而且函数返回时里面的帧会全没。
*/
static uint8_t ota_block[OTA_BLOCK_SIZE] __attribute__((aligned(4)));

/* ------------------------------------------------------------------ */
/* 辅助函数                                                            */
/* ------------------------------------------------------------------ */

/*
    把 buf 里的 len 字节写进中转区的 addr。
    写之前先补擦 —— Flash 只能把 1 写成 0，不能把 0 变回 1，
    往没擦过的区域写，新数据会和旧数据按位与，得到的既不是新也不是旧，而且 gd25_write 还会返回"成功"。
    addr 是绝对地址；擦除按 4 KiB 扇区走，所以 erased_bytes 记的是"已经擦到哪了"。
*/
static uint8_t ota_flash_write(const uint8_t *buf, uint16_t len, uint32_t addr)
{
    uint16_t off = 0U;
    uint32_t need_erased = (addr - OTA_RELAY_IMAGE_BASE) + (uint32_t)len;

    while (ota_state.erased_bytes < need_erased)
    {
        if (gd25_clear(OTA_RELAY_IMAGE_BASE + ota_state.erased_bytes) == 0U)
        {
            return 0U;
        }
        ota_state.erased_bytes += GD25Q32_SECTOR_SIZE;
    }

    /* gd25_write 一次最多写一页，跨过 256 字节边界它会直接拒绝。所以要自己切成小块。 */
    while (off < len)
    {
        uint16_t chunk = (uint16_t)(len - off);

        if (chunk > GD25Q32_PAGE_SIZE)
        {
            chunk = GD25Q32_PAGE_SIZE;
        }
        if (gd25_write(addr + (uint32_t)off, buf + off, chunk) == 0U)
        {
            return 0U;
        }
        off = (uint16_t)(off + chunk);
    }

    return 1U;
}

/*
    解析镜像头，拿到"该收多少"和"该是什么 CRC"。
    镜像头在传输的最前面（Hub 从镜像第 0 字节开始发），
    256 < 768，所以第一次刷写之前它就已经躺在 ota_block 里了 —— 不用去 Flash 里读。
    返回 0 表示这个头不认，整趟传输作废。
*/
static uint8_t ota_header_parse(void)
{
    const ota_image_header_t *header = (const ota_image_header_t *)ota_block;
    uint32_t image_total;

    /*
        判据必须和 bootloader 的 image_verify.c 完全一致（项目、顺序都一样）。
        同一份镜像在两处被验，判据分叉就是"设备说好、bootloader 说坏"的来源。
    */
    if (header->magic != OTA_IMAGE_MAGIC)
    {
        return 0U;
    }
    if (header->header_version != OTA_IMAGE_FORMAT_VERSION)
    {
        return 0U;
    }
    if ((uint32_t)header->header_size != (uint32_t)OTA_IMAGE_HEADER_SIZE)
    {
        return 0U;
    }

    /*
        头自身的 CRC —— 这一关必须在采信任何数字之前过。
        只验 magic 是挡不住的：magic 那 4 个字节碰巧对，application_length 照样可能是垃圾。
    */
    if (crc32_check((const uint8_t *)header,
                    OTA_IMAGE_HEADER_CRC_LENGTH,
                    header->header_crc32) == 0U)
    {
        return 0U;
    }

    if (header->hardware_model != OTA_CURRENT_HARDWARE_MODEL)
    {
        return 0U;
    }

    /* 上下界都要查，而且必须在后面用它之前查：坏掉的长度会让写入地址跑出中转区。 */
    if (header->application_length < OTA_APPLICATION_MIN_LENGTH)
    {
        return 0U;
    }

    image_total = OTA_IMAGE_HEADER_SIZE + header->application_length;
    if (image_total > OTA_RELAY_IMAGE_CAPACITY)
    {
        return 0U;
    }

    ota_state.image_total = image_total;
    ota_state.image_crc32 = header->application_crc32;

    return 1U;
}

/*
    收完了吗？
    用 >= 不用 ==：每帧固定送 6 字节，而 image_total 不一定是 6 的整数倍。
    比如 image_total = 1000，收到的字节数是 996 -> 1002，中间根本不会等于 1000。
    末帧多出来的那几个字节是填充，收尾刷最后一块时会被 image_total 截掉。
*/
static uint8_t ota_receive_finished(void)
{
    return (ota_state.received_bytes >= ota_state.image_total) ? 1U : 0U;
}

/*
    收满总长之后跑一次：
    ① 刷最后一块（那个不满 768 的尾巴，还可能带末帧填充）
    ② 读回中转区算 CRC，和镜像头里的比对
    返回 1 = 校验通过，镜像已经完整地躺在中转区里。

    为什么要读回：验的是"真的写进 Flash 的东西"。
    只在 RAM 里算，验不出 Flash 写坏 —— 而写坏正是这一步最该抓的。
    （bootloader 搬运前还会再验一遍，因为中间隔着一次复位，那段时间它有它要负责的区间。）
*/
static uint8_t ota_finalize(void)
{
    uint32_t tail = ota_state.image_total - ota_state.written_bytes;
    uint32_t crc;
    uint32_t addr;
    uint32_t remain;

    if (tail > (uint32_t)ota_state.block_bytes)
    {
        return 0U;      /* 状态对不上，别往 Flash 里写乱七八糟的东西 */
    }

    if (tail > 0U)
    {
        if (ota_flash_write(ota_block,
                            (uint16_t)tail,
                            OTA_RELAY_IMAGE_BASE + ota_state.block_offset) == 0U)
        {
            return 0U;
        }
        ota_state.block_offset += tail;
        ota_state.written_bytes += tail;
        ota_state.block_bytes = 0U;
    }

    /*
        镜像头里的 application_crc32 只覆盖应用本体，不含那 256 字节头，所以从 +256 开始算。
        ota_block 这会儿已经空闲了，借它当读回缓冲，省 768 字节 RAM。
    */
    crc = crc32_begin();
    addr = OTA_RELAY_IMAGE_BASE + OTA_IMAGE_HEADER_SIZE;
    remain = ota_state.image_total - OTA_IMAGE_HEADER_SIZE;

    while (remain > 0U)
    {
        uint16_t chunk = (remain > GD25Q32_PAGE_SIZE) ? GD25Q32_PAGE_SIZE : (uint16_t)remain;

        if (gd25_read(addr, ota_block, chunk) == 0U)
        {
            return 0U;
        }
        crc = crc32_update(crc, ota_block, chunk);
        addr += (uint32_t)chunk;
        remain -= (uint32_t)chunk;
    }

    return (crc32_finish(crc) == ota_state.image_crc32) ? 1U : 0U;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

/* 把状态清回初始。全 0 就是合法初始态（见上面 ota_rx_state_t 的注释），所以一句话就够。 */
void ota_reset(void)
{
    memset(&ota_state, 0, sizeof(ota_state));
}

/*
    这个函数处理一个固件帧，然后返回下一步需要干什么
*/
ota_result_t ota_on_frame(uint16_t standard_id,const uint8_t data[OTA_FRAME_LENGTH],uint8_t data_length)
{
    ota_result_t result;
    uint16_t raw_seq;

    /* result 是"这一次调用的输出"，所以它是局部的。
       做成文件级的话，ota_on_idle 会返回"上一帧留下的结论"。 */
    memset(&result, 0, sizeof(result));

    /* 已经收完并校验通过了：后面再来的帧一律不理。
       少了这一句，Hub 窗口尾巴多送几帧，就会把 ota_finalize 从头再跑一遍
       —— 重读 100+ KB、重算一次 CRC，而且数据会一路写到镜像后面去。 */
    if(ota_state.done != 0U)
    {
        return result;
    }

    /* 第一关：长度。上游验过不等于不会错，这一层是真正要动 Flash 的，自己再验一遍。 */
    if(data_length != OTA_FRAME_LENGTH)
    {
        return result;
    }

    /* 第二关：归属。CAN 是广播的，别人升级时他的固件帧也会进这条队列。
       少了这一关，别人升级会把他的固件写进本节点的中转区。 */
    if(standard_id != (CAN_OTA_DATA_BASE_ID + board_id))
    {
        return result;
    }

    raw_seq = ((uint16_t)data[0] << 8) | (uint16_t)data[1];

    /* 第三关：序号。断号、重复、乱序三种情况全被这一句挡掉，
       而且 expected_seq 一个字都不动 —— 这就是"回退 N"接收侧的全部。 */
    if(raw_seq != ota_state.expected_seq)
    {
        return result;
    }

    /* 三关都过了：这一帧是要的 */
    memcpy(&ota_block[ota_state.block_bytes], &data[2], OTA_FRAME_PAYLOAD);
    ota_state.block_bytes   += OTA_FRAME_PAYLOAD;
    ota_state.expected_seq  += 1U;
    ota_state.received_bytes += OTA_FRAME_PAYLOAD;
    ota_state.idle_ms = 0U;     /* 有帧进来，说明传输还活着 */

    /* 镜像头解析：收满 256 字节就可以读了，只做一次。 */
    if((ota_state.image_total == 0U) && (ota_state.received_bytes >= OTA_IMAGE_HEADER_SIZE))
    {
        if(ota_header_parse() == 0U)
        {
            ota_reset();        /* 头都不认，这趟传输作废 */
            return result;
        }
    }

    /* 攒满一块就刷进中转区 */
    if(ota_state.block_bytes == OTA_BLOCK_SIZE)
    {
        if(ota_flash_write(ota_block,
                           OTA_BLOCK_SIZE,
                           OTA_RELAY_IMAGE_BASE + ota_state.block_offset) == 0U)
        {
            ota_reset();        /* Flash 写失败：放弃本次升级，别卡死 */
            return result;
        }
        ota_state.block_offset += OTA_BLOCK_SIZE;
        ota_state.written_bytes += OTA_BLOCK_SIZE;
        ota_state.block_bytes = 0U;
    }

    /* 收满总长：刷尾巴 + 读回校验 */
    if((ota_state.image_total != 0U) && (ota_receive_finished() != 0U))
    {
        if(ota_finalize() != 0U)
        {
            ota_state.done  = 1U;   /* 停手，别再往后收 */
            result.finished = 1U;
            result.success  = 1U;
            /* Task 8 在此接上：写「待装」标志，然后复位整机。 */
        }
        else
        {
            ota_reset();        /* 校验没过：清干净等下一趟，绝不能卡死 */
        }
    }

    return result;
}

/*
    闲着的时候问一句（外壳在队列超时时调）。
    ota.c 拿不到时钟，所以只能数"被叫了多少次"：
    每次 ≈ OTA_IDLE_REPLY_MS 毫秒，累计到 OTA_TRANSFER_TIMEOUT_MS 就推断传输已经死了。

    为什么要有这一条：Hub 传到一半自己挂了，它什么都不发。
    设备看到的世界只是"安静"，和"Hub 正在准备下一批"长得一模一样 ——
    所以只能自己数时间推断，跟 bootloader 数试启动次数是同一个形状。
*/
ota_result_t ota_on_idle(void)
{
    ota_result_t result;

    memset(&result, 0, sizeof(result));

    /* 还没开始收，不用计时。 */
    if(ota_state.received_bytes == 0U)
    {
        return result;
    }

    ota_state.idle_ms += OTA_IDLE_REPLY_MS;

    if(ota_state.idle_ms >= OTA_TRANSFER_TIMEOUT_MS)
    {
        ota_reset();            /* 推断：这次传输已经废了 */
    }

    return result;
}
