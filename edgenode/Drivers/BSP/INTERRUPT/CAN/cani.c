#include "cani.h"
#include "FreeRTOS.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Protocol/Can/can_frame.h"
#include "gd32f10x.h"
#include "gd32f10x_can.h"

//对于接受层，我们只做数据存储不做帧协议解析
static can_receive_message_struct can0_receive_message;

void USBD_LP_CAN0_RX0_IRQHandler()
{
    uint8_t index;
    BaseType_t higher_priority_task_woken = pdFALSE;
    can_receive_frame_t received_frame;

    if(can_interrupt_flag_get(CAN0, CAN_INT_FLAG_RFL0) == SET)
    {
        can_message_receive(CAN0, CAN_FIFO0, &can0_receive_message);

        /* ACK 只允许标准数据帧进入任务队列，远程帧和扩展帧直接丢弃。 */
        if((can0_receive_message.rx_ff == CAN_FF_STANDARD) &&
           (can0_receive_message.rx_ft == CAN_FT_DATA))
        {
            received_frame.standard_id = (uint16_t)can0_receive_message.rx_sfid;
            received_frame.data_length = can0_receive_message.rx_dlen;
            for(index = 0U; (index < received_frame.data_length) && (index < 8U); index++)
            {
                received_frame.data[index] = can0_receive_message.rx_data[index];
            }

            /*
                固件帧和遥测/ACK 不能共用一个队列：队列是"取走即消失"的，
                两个任务抢读会互相偷帧（遥测 ACK 被 OtaTask 吃掉，或反之）。

                OTA 数据帧和控制帧走 OTA 队列，其余帧走普通 CAN 队列。别人的固件帧也会进来
                （CAN 是广播的），由 OtaTask 按节点号丢弃 —— 中断里不查节点号，
                这样中断不必知道 board_id，遥测那条路也一个字不改。
            */
            if(((received_frame.standard_id >= CAN_OTA_DATA_BASE_ID) &&
                (received_frame.standard_id <
                 (CAN_OTA_DATA_BASE_ID + CAN_ID_SEGMENT_SIZE))) ||
               ((received_frame.standard_id >= CAN_OTA_CTRL_BASE_ID) &&
                (received_frame.standard_id <
                 (CAN_OTA_CTRL_BASE_ID + CAN_ID_SEGMENT_SIZE))))
            {
                (void)rtos_ota_frame_send_from_isr(&received_frame,
                                                   &higher_priority_task_woken);
            }
            else
            {
                (void)rtos_can_receive_frame_send_from_isr(&received_frame,
                                                           &higher_priority_task_woken);
            }

            /* 两条路共同的后继动作：谁被唤醒都置同一位，所以放在分支外。 */
            portYIELD_FROM_ISR(higher_priority_task_woken);
        }
    }
}
