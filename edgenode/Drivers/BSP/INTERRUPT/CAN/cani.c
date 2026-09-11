#include "cani.h"
#include "gd32f10x.h"
#include "gd32f10x_can.h"

//对于接受层，我们只做数据存储不做帧协议解析
static can_receive_message_struct can0_receive_message;

void USBD_LP_CAN0_RX0_IRQHandler()
{
    if(can_interrupt_flag_get(CAN0, CAN_INT_FLAG_RFL0) == SET)
    {
        can_message_receive(CAN0, CAN_FIFO0, &can0_receive_message);
    }
}
