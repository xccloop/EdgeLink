#ifndef HMI_CONTROL_H_
#define HMI_CONTROL_H_
#include "Presentation/Hmi/hmi_types.h"
/*
    HMI唯一业务入口。所有接口必须由同一执行上下文串行调用，不能从ISR调用；
    将来任务接入时，业务数据和按键应先交给拥有HMI的任务。
    先由外部完成ips_init/display_buffer_init，再调用hmi_init一次。
    此模块不初始化外设，不创建任务，不接收业务队列。
*/
void hmi_init(uint8_t node_id);
/* 复制整份快照；无效状态/空指针/未初始化返回0，不修改原状态。 */
uint8_t hmi_set_view_data(const hmi_view_data_t *data);
/* 接收已经消抖的单次按键事件，不读取GPIO、不自动重复按键。 */
void hmi_key_event(hmi_key_t key);
/* 周期调用；即使页面不变也必须调用。返回0为错误锁存，1为本次服务正常。
   返回1不表示屏幕已经刷完，不得据此停掉后续service。 */
uint8_t hmi_service(void);
/* 显式解除绘制错误并请求整屏重画；不复位硬件。 */
void hmi_refresh(void);
hmi_page_id_t hmi_current_page(void);
hmi_page_id_t hmi_selected_page(void);
#endif
