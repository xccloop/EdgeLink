#include "usart.h"
#include "gd32f10x.h"
#include "gd32f10x_usart.h"
#include <stdint.h>

/*
    这里我们注意到我们使用volatile来修饰我们的接受数据变量，我们来细说一下vloliate是什么
    编译器在运行的过程中偶尔会将某些变量进行优化和修改，而如果这个变量我们不希望他修改，比如接受的数据，这种修改以后会造成数据可靠性丢失的问题
    我们就使用voliate来进行数据的修饰
    后续要extern ch340_receive_data声明外部已经定义了这个数据，这样我们才可以确保数据是串口中断函数中来的
*/
volatile uint8_t ch340_receive_data = 0;

void  USART0_IRQHandler()
{
    //这里我们获取usart0的中断标志位，第二个形参代表Read Buffer Not Empty，即读取缓存器非空，说明此时有数据来了
    if(usart_interrupt_flag_get(USART0,USART_INT_FLAG_RBNE) == SET)
    {
        //调用函数接受数据
        //串口接受是逐字节的，因此这里我们只实现将单次接受到的单个字节存储到单个字节中，后续这个数据要怎么使用是应用层考虑的问题
        //不在本次函数讨论范围之内
        ch340_receive_data = (uint8_t)usart_data_receive(USART0);
    }
}