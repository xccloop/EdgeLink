#ifndef BOOT_SCREEN_H_
#define BOOT_SCREEN_H_

#include <stdint.h>

/*
    卡住时显示的那一屏。

    对应应用的 User/App/Presentation/Hmi/Page/hmi_page.c —— 那边每次刷新
    都按温度/WiFi/日志重建页面；这边只有一屏，定义一次就够。

    布局是固定的"体检表"：标题 + 元数据状态 + 两个槽的状态 + 原因说明。
    不同的空转原因只改【原因说明】那两行文字 —— 状态区是现场实测的，
    不管哪种原因都照实显示。

    调用前必须先完成 ips_init() 和 display_buffer_init()。
    本函数会同步刷完整屏（约 137ms）才返回。
*/

/* 卡住的原因。每一种对应屏幕上不同的说明文字。 */
typedef enum
{
    /* 元数据不可用，而且 A / B 两个槽都挑不出能用的 */
    BOOT_HALT_NO_USABLE_IMAGE = 0,

    /* 元数据说该跳某个槽，但它校验不过，而且没有回退目标 */
    BOOT_HALT_TARGET_INVALID,

    /* 待试启动槽坏了，回退过去的活动槽也坏了 */
    BOOT_HALT_FALLBACK_INVALID,

    /* boot_jump_to_vector() 居然返回了 —— 理论上不可能 */
    BOOT_HALT_JUMP_RETURNED,

    BOOT_HALT_REASON_COUNT
} boot_halt_reason_t;

/* 屏幕上那份"体检表"的内容 */
typedef struct
{
    boot_halt_reason_t reason;
    uint8_t            meta_ok;    /* 元数据读到真实记录了吗 */
    uint8_t            slot_a_ok;  /* 槽 A 通过 image_verify 了吗 */
    uint8_t            slot_b_ok;  /* 槽 B 通过 image_verify 了吗 */
} boot_halt_info_t;

void boot_screen_show_halt(const boot_halt_info_t *info);

#endif
