#ifndef TCP_RECEIVE_H_
#define TCP_RECEIVE_H_

/* 在 TCP 初始化前注册接收回调；业务组帧和 FromISR 入队都由 APP 负责。 */
void tcp_receive_init(void);

#endif
