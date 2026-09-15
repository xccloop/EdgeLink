#include "transmit.h"
#include <stdint.h>

#include "Protocol/Tcp/tcp_frame.h"
#include "Output/Tcp/tcp.h"

/*
    这一层未来负责协调TCP和CAN输出。
    当前ESP-AT的数据发送步骤和CAN分支接入尚未完成，不能把“编码完成”当作“发送成功”。
*/

/*
    首先我们思考一下数据传输，我们应当是传入一个message，然后回转为frame自定义帧，我们在通过AT指令发送
*/
uint8_t tcp_frame_transmit(telemetry_sample_struct *message,uint16_t sequence)
{
    uint8_t transmit_data[16];

    if(tcp_frame_encode(transmit_data,sequence,message) == 0U)
    {
        return TCP_TRANSMIT_FAIL;
    }

    if(tcp_send(transmit_data, sizeof(transmit_data)) == TCP_SEND_FAIL)
    {
        return TCP_TRANSMIT_FAIL;
    }

    return TCP_TRANSMIT_SUCCESS;
}
uint8_t can_frame_transmit()
{


    /* CAN输出需要Message、序号和节点ID；接口补齐前不报告发送成功。 */
    return CAN_TRANSMIT_FAIL;
}
