#ifndef IPS_H_
#define IPS_H_

#include <stdint.h>

/* 返回1表示ST7789初始化命令已发送完成；返回0表示SPI发送超时，不能开始刷屏。 */
uint8_t ips_init(void);

/* 我们要的是横屏效果，显示层使用X=0~319、Y=0~239的坐标。 */
#define IPS_WIDTH                     320U
#define IPS_HEIGHT                    240U
#define IPS_RGB565_BYTES_PER_PIXEL    2U
#define IPS_DMA_LINE_BYTES            (IPS_WIDTH * IPS_RGB565_BYTES_PER_PIXEL)
#define IPS_SUCCESS                   1U
#define IPS_FAIL                      0U

/*
    一次提交一行RGB565像素。APP只需要提供像素数据和它要显示的位置，
    IPS内部会完成窗口设置、DMA启动和上一笔传输的状态检查。

    返回IPS_FLUSH_ACCEPTED时，data已经交给DMA，APP暂时不能改写这块缓冲区；
    data为NULL时不提交新行，只查询并收尾上一笔DMA，x、y、width会被忽略。
    这让APP在没有新行需要发送时，也能确认最后一行已经完成。

    返回IPS_FLUSH_BUSY时，上一行还在发送，本行尚未开始；
    返回IPS_FLUSH_COMPLETE时，查询到上一行已经完成；
    返回IPS_FLUSH_ERROR时，本次没有成功交给DMA，APP可以处理或丢弃该行数据。
*/
typedef enum
{
    IPS_FLUSH_ACCEPTED = 0U,
    IPS_FLUSH_BUSY,
    IPS_FLUSH_COMPLETE,
    IPS_FLUSH_ERROR
} ips_flush_result_enum;

ips_flush_result_enum ips_flush_line(const uint8_t *data, uint16_t x, uint16_t y, uint16_t width);

#endif
