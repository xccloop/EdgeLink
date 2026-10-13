#ifndef TCP_RECEIVE_H_
#define TCP_RECEIVE_H_

/* 在 TCP 初始化前注册接收回调；业务组帧和 FromISR 入队都由 APP 负责。 */
void tcp_receive_init(void);

/* 仅 OtaTask 调用：完整块消费后归还缓冲；周期处理半包超时。 */
void tcp_receive_block_release(void);
void tcp_receive_service(void);

#endif
