#include "flash_layout.h"   /* OTA_SLOT_A_BASE_ADDRESS / OTA_SLOT_B_BASE_ADDRESS */
#include "ota_image.h"      /* ota_image_header_t / OTA_APPLICATION_OFFSET */
#include "ota_metadata.h"   /* ota_metadata_t / load / save / OTA_SLOT_NONE */
#include "boot_jump.h"
#include "image_verify.h"
#include "CH340/ch340.h"
#include "board_time.h"
#include "board_debug.h"
#include "IPS/ips.h"
#include "Display/display_buffer.h"
#include "boot_screen.h"
#include <stdbool.h>
#include <stdint.h>

/*
    Bootloader 主流程。

    它上电后只回答一个问题：这次该启动哪个槽？

    贯穿全局的一条原则：

        元数据在  →  听元数据（那是"用户的决定"）
        元数据不在 →  看镜像   （那是"固件的属性"）

    为什么不能反过来：版本号是固件自己的属性，"谁版本大"回答不了
    "用户想启动谁"。降级时用户明明想启动旧版本，比版本号会选错；
    试启动失败回退后，比版本号又会把坏固件选回来，变成无限重启。

    所以版本号只在一种场合出场：元数据整个丢了、没有任何决定可依据时，
    用它来猜"用户最后一次升级大概留下了哪个"。那是灾后恢复，不是日常决策。

    分工：
        image_verify          判断"这个槽能不能用"，从不决定"该跳哪个"
        本文件                 拿这个裁决去组合出"该怎么办"

    日志一律走 ch340_* 那组函数，不用 printf ——
    见 ch340.h 里对"为什么不用 printf"的说明。
*/

/* 连续试启动失败多少次就放弃新槽、回退到活动槽。 */
#define OTA_BOOT_ATTEMPT_LIMIT  3U

/* 打一行"标签 + 8 位十六进制地址"。槽地址的日志基本都长这样。 */
static void boot_log_addr(const char *label, uint32_t slot)
{
    ch340_puts(label);
    ch340_put_hex32(slot);
    ch340_puts("\r\n");
}

/* 元数据到底读没读到 —— 主流程记下来，卡住时屏幕要显示它。 */
static uint8_t boot_meta_was_real;

/*
    每种故障对应的串口日志文字。

    和屏幕上那套（boot_screen.c 里的 screen_messages）故意分开：
    日志是给开发者看的，屏幕是给现场人员看的，措辞本来就不该一样。
*/
static const char *const boot_halt_log[BOOT_HALT_REASON_COUNT] =
{
    "no usable image in A or B",
    "active slot invalid, no fallback",
    "pending invalid, fallback also invalid",
    "jump returned unexpectedly",
};

/*
    卡住时的落点。

    不是"出错罢工"，而是"等待救援"：后续接入 CAN 固件接收后，
    这个循环就是等待从总线灌入固件的地方。设备停在这里仍然活着，能被远程救回来。

    停下来之前做两件事：
      1. 串口打一行日志并等它发完 —— 否则设备静默卡死，
         你完全分不清它是卡在这里、卡在校验里、还是压根没启动。
      2. 点亮屏幕，把故障说明和两个槽的实测状态摆出来。
*/
static void boot_halt(boot_halt_reason_t reason)
{
    boot_halt_info_t info;

    if (reason >= BOOT_HALT_REASON_COUNT)
    {
        reason = BOOT_HALT_JUMP_RETURNED;   /* 不该发生，兜一下防止数组越界 */
    }

    ch340_puts("[boot] HALT: ");
    ch340_puts(boot_halt_log[reason]);
    ch340_puts("\r\n");
    ch340_flush();

    /*
        屏幕上那份"体检表"是【现场实测】的，不是主流程一路传下来的 ——
        卡住之后重新验一遍，屏幕说的就是此刻的真实情况。
        两个槽各算一次 CRC 约 80ms；反正已经卡住了，这点时间不算什么。
    */
    info.reason    = reason;
    info.meta_ok   = boot_meta_was_real;
    info.slot_a_ok = (image_verify(OTA_SLOT_A_BASE_ADDRESS) != false) ? 1U : 0U;
    info.slot_b_ok = (image_verify(OTA_SLOT_B_BASE_ADDRESS) != false) ? 1U : 0U;

    /*
        卡住了才点屏 —— 现场设备屏幕是唯一的人机界面，黑屏等于什么都没说。

        为什么放在这里而不是 main 开头：
          ips_init() 里有约 490ms 的固定延时（屏幕复位 + 初始化序列），
          每次上电都点屏会让正常启动白等半秒，还会闪一下。
          只有真的卡住了才付这个时间。

        屏幕坏掉不该把串口那条路也堵死：
        ips_init() 内部有 SPI 超时保护，失败返回 IPS_FAIL，
        那就跳过刷屏，日志照旧发得出去。
    */
    /*
        点屏前的两件准备，顺序不能反，也不能省：

        ① board_debug_init()：PB3 复位后是 JTDO，不关掉 JTAG 就不能当
           SPI2_SCK 用 —— 屏幕会全黑。而且 ips_init() 还不一定报错，
           因为 SPI 硬件自己是配置"成功"的，只是时钟线根本没接出来。
        ② board_systick_init()：ips_init() 内部要用 delay_ms() 等屏幕复位，
           而 delay_ms() 靠 SysTick 计数 —— 没有它就会死等在那里。

        这两条应用里都由 board_config_init() 代劳，Bootloader 必须自己补。
    */
    board_debug_init();
    board_systick_init();

    if (ips_init() == IPS_SUCCESS)
    {
        (void)display_buffer_init();
        boot_screen_show_halt(&info);
        ch340_puts("[boot] screen up\r\n");
    }
    else
    {
        /* ips_init() 内部有 SPI 超时保护；失败时屏幕点不亮，但得让人知道。 */
        ch340_puts("[boot] display init FAILED, screen stays dark\r\n");
    }

    ch340_flush();

    while (1)
    {
    }
}

/*
    读一个槽镜像头里的固件版本号。

    调用前必须确认该槽已通过 image_verify —— 校验不过的槽，
    镜像头里读出来的是垃圾，比大小毫无意义。
*/
static uint32_t boot_slot_version(uint32_t slot_base)
{
    const ota_image_header_t *header;

    header = (const ota_image_header_t *)slot_base;

    return header->firmware_version;
}

/*
    元数据不可用时的兜底：不猜，直接看两个槽谁真的能用。

    返回一个【已经通过 image_verify】的槽基地址；
    两个都不能用时返回 0。

    只有两个槽都能用时才需要比版本号 —— 那时的含义是
    "没有决定可依据，那就挑用户最后一次升级留下的那个"。
*/
static uint32_t boot_choose_usable_slot(void)
{
    uint8_t a_ok;
    uint8_t b_ok;

    a_ok = image_verify(OTA_SLOT_A_BASE_ADDRESS);

    if (a_ok == 0U)
    {
        /* 槽 A 不可用 —— 只剩槽 B 有机会，没必要再比版本号。 */
        b_ok = image_verify(OTA_SLOT_B_BASE_ADDRESS);

        return (b_ok != 0U) ? OTA_SLOT_B_BASE_ADDRESS : 0U;
    }

    b_ok = image_verify(OTA_SLOT_B_BASE_ADDRESS);

    if (b_ok == 0U)
    {
        /* 槽 B 不可用 —— 只剩槽 A。 */
        return OTA_SLOT_A_BASE_ADDRESS;
    }

    /* 两个都能用：挑版本号大的，那多半是用户最后一次升级留下的。 */
    ch340_puts("[boot] both slots valid, A=v");
    ch340_put_dec(boot_slot_version(OTA_SLOT_A_BASE_ADDRESS));
    ch340_puts(" B=v");
    ch340_put_dec(boot_slot_version(OTA_SLOT_B_BASE_ADDRESS));
    ch340_puts("\r\n");

    return (boot_slot_version(OTA_SLOT_B_BASE_ADDRESS) >
            boot_slot_version(OTA_SLOT_A_BASE_ADDRESS)) ?
           OTA_SLOT_B_BASE_ADDRESS : OTA_SLOT_A_BASE_ADDRESS;
}

int main(void)
{
    ota_metadata_t meta;
    uint32_t target_slot;
    uint32_t fallback_slot;
    uint8_t load_result;
    uint8_t had_pending;

    /* 串口必须最先初始化，后面每一步才有人听得到。 */
    ch340_init();
    ch340_puts("\r\n[boot] EdgeNode bootloader\r\n");

    load_result = ota_metadata_load(&meta);

    /* 记下来：卡住时屏幕上的"体检表"要显示元数据到底读没读到。 */
    boot_meta_was_real = (load_result == OTA_METADATA_LOAD_REAL) ? 1U : 0U;

    if (load_result == OTA_METADATA_LOAD_REAL)
    {
        /*
            ———— 有记事本：听记事本 ————

            两份记录里读到了真实内容，说明"用户上次选了谁"是已知的。
            下面全部按元数据走，不看版本号。
        */
        ch340_puts("[boot] meta REAL active=");
        ch340_put_hex32(meta.active_slot);
        ch340_puts(" pending=");
        ch340_put_hex32(meta.pending_slot);
        ch340_puts(" attempts=");
        ch340_put_dec(meta.boot_attempts);
        ch340_puts("\r\n");

        had_pending = (meta.pending_slot != OTA_SLOT_NONE) ? 1U : 0U;

        /* 0 表示"目标槽校验不过就停住，不换别的槽"。 */
        fallback_slot = 0U;

        if (had_pending != 0U)
        {
            /*
                有"待试启动"：上一次升级已经写完新固件并通过校验。

                先把这次尝试记下来，再跳。顺序绝对不能反 ——
                如果等新应用启动成功后再补写，"应用起不来"这种情况
                永远不会被计数，回退永远触发不了，设备会卡在
                "试启动 → 死机 → 复位 → 再试启动"的循环里。
            */
            meta.boot_attempts++;

            if (meta.boot_attempts <= OTA_BOOT_ATTEMPT_LIMIT)
            {
                target_slot = meta.pending_slot;

                /*
                    只有"从待试启动槽退到活动槽"这一条回退路。
                    待试槽是刚写进去、还没被验证过的新东西，它出问题很正常。
                */
                fallback_slot = meta.active_slot;

                ch340_puts("[boot] try #");
                ch340_put_dec(meta.boot_attempts);
                ch340_puts(" -> ");
                ch340_put_hex32(target_slot);
                ch340_puts("\r\n");
            }
            else
            {
                /*
                    试了这么多次，新应用一次都没来销账，判定它起不来：
                    清掉待试启动、计数归零，回到原活动槽。

                    注意这里只改元数据，没有动任何一个槽里的固件 ——
                    新固件仍完整地留在那儿，下次升级可以覆盖它。
                */
                meta.pending_slot = OTA_SLOT_NONE;
                meta.boot_attempts = 0U;
                target_slot = meta.active_slot;

                ch340_puts("[boot] attempts exhausted, give up pending\r\n");
            }
        }
        else
        {
            /*
                没有待试启动，目标槽就是活动槽。

                活动槽本来不该被碰（升级永远只写非活动槽），
                它校验不过属于异常。这里【不换槽】：换到另一个槽去，
                万一那个槽"校验能过但一跑就死机"，就会变成无限重启循环。
                停下来等救援，比循环重启更可预测。
            */
            target_slot = meta.active_slot;
        }

        if (image_verify(target_slot) == false)
        {
            boot_log_addr("[boot] INVALID image at ", target_slot);

            if (fallback_slot == 0U)
            {
                boot_halt(BOOT_HALT_TARGET_INVALID);
            }

            /* 待试启动槽不可用 = 升级没写成功 → 清掉 pending，退回活动槽。 */
            target_slot = fallback_slot;
            meta.pending_slot = OTA_SLOT_NONE;
            meta.boot_attempts = 0U;

            boot_log_addr("[boot] fallback -> ", target_slot);

            if (image_verify(target_slot) == false)
            {
                boot_halt(BOOT_HALT_FALLBACK_INVALID);
            }
        }

        /*
            只有动过记录才写回。没有待试启动时这份记录一个字都没改，
            就不该去擦 Flash —— 每擦一次都是一次掉电风险，也是 Flash 寿命。

            写回失败也不阻断启动：最坏结果是"这次尝试没被记账"，
            下次上电可能多试一次；但绝不会因为状态写不进去就把设备卡住。
        */
        if (had_pending != 0U)
        {
            (void)ota_metadata_save(&meta);
        }
    }
    else
    {
        /*
            ———— 没有记事本：不猜，去看两个槽谁真的能用 ————

            走到这里意味着：全新设备（Flash 全 0xFF），
            或者两份记录都被写坏了。

            旧做法是"固定默认槽 A" —— 那等于在没有依据的时候硬选一个，
            设备明明跑的是 B 也会被猜成 A。这里改成实地去看。
        */
        ch340_puts("[boot] meta UNREADABLE, picking from slots\r\n");

        target_slot = boot_choose_usable_slot();

        if (target_slot == 0U)
        {
            boot_halt(BOOT_HALT_NO_USABLE_IMAGE);
        }

        /*
            把"实地看出来的"结果写回，下次上电就不用再挑一遍。

            写入被打断也没有额外损失：两份记录本来就是坏的，
            下次照旧走这条兜底路，最坏等于没修。
        */
        meta.active_slot = target_slot;
        meta.pending_slot = OTA_SLOT_NONE;
        meta.boot_attempts = 0U;
        meta.pending_version = 0U;
        (void)ota_metadata_save(&meta);

        boot_log_addr("[boot] rebuilt meta, active=", target_slot);
    }

    /*
        走到这里时 target_slot 必定已经通过 image_verify：

          - 上面那条路：跳转前刚校验过，校验不过的已经被 boot_halt 拦住
          - 下面那条路：boot_choose_usable_slot() 只返回它亲自校验通过的槽

        元数据只给"候选"，image_verify 才是"裁决"。
    */
    boot_log_addr("[boot] jump -> ", target_slot + OTA_APPLICATION_OFFSET);

    /*
        跳转前必须把串口发干净。
        每个字节只等到 TBE（数据寄存器空），最后一个字节可能还卡在移位寄存器里；
        应用启动后会 usart_deinit(USART0)，那一刻未发完的字节就被截断。
    */
    ch340_flush();

    boot_jump_to_vector(target_slot + OTA_APPLICATION_OFFSET);

    /* boot_jump_to_vector 是 noreturn，正常走不到这里。 */
    boot_halt(BOOT_HALT_JUMP_RETURNED);
    return 0;
}
