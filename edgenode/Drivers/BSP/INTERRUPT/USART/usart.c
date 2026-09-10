#include "usart.h"
#include "gd32f10x.h"
#include "gd32f10x_usart.h"

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

/*
    这个中断函数服务于ESP12S,用于接受ESP12S发送的返回信息
    这里我们的设想是暴露给外部接口的是AT回应的状态，我们采用流失状态机，说白了就是一步一步的往下判断到底属于什么
*/

volatile esp12s_response_t esp12s_receive_data = ESP12S_RESPONSE_NONE;

typedef enum
{
    ESP12S_MATCH_IDLE = 0,
    ESP12S_MATCH_O,
    ESP12S_MATCH_OK,
    ESP12S_MATCH_OK_CR,
    ESP12S_MATCH_E,
    ESP12S_MATCH_ER,
    ESP12S_MATCH_ERR,
    ESP12S_MATCH_ERRO,
    ESP12S_MATCH_ERROR,
    ESP12S_MATCH_ERROR_CR
} esp12s_match_state_t;

static esp12s_match_state_t esp12s_match_state = ESP12S_MATCH_IDLE;
static uint8_t esp12s_line_start = 1;

void USART1_IRQHandler()
{
    uint8_t receive_data;

    if(usart_interrupt_flag_get(USART1,USART_INT_FLAG_RBNE) == SET)
    {
        receive_data = (uint8_t)usart_data_receive(USART1);

        switch (esp12s_match_state)
        {
            case ESP12S_MATCH_IDLE:
                if((esp12s_line_start != 0) && (receive_data == 'O'))
                {
                    esp12s_match_state = ESP12S_MATCH_O;
                    esp12s_line_start = 0;
                }
                else if((esp12s_line_start != 0) && (receive_data == 'E'))
                {
                    esp12s_match_state = ESP12S_MATCH_E;
                    esp12s_line_start = 0;
                }
                else
                    esp12s_line_start = (receive_data == '\n');
                break;

            case ESP12S_MATCH_O:
                if(receive_data == 'K')
                    esp12s_match_state = ESP12S_MATCH_OK;
                else
                {
                    esp12s_match_state = ESP12S_MATCH_IDLE;
                    esp12s_line_start = (receive_data == '\n');
                }
                break;

            case ESP12S_MATCH_OK:
                if(receive_data == '\r')
                    esp12s_match_state = ESP12S_MATCH_OK_CR;
                else
                {
                    esp12s_match_state = ESP12S_MATCH_IDLE;
                    esp12s_line_start = (receive_data == '\n');
                }
                break;

            case ESP12S_MATCH_OK_CR:
                if(receive_data == '\n')
                    esp12s_receive_data = ESP12S_RESPONSE_OK;

                esp12s_match_state = ESP12S_MATCH_IDLE;
                esp12s_line_start = (receive_data == '\n');
                break;

            case ESP12S_MATCH_E:
                if(receive_data == 'R')
                    esp12s_match_state = ESP12S_MATCH_ER;
                else
                {
                    esp12s_match_state = ESP12S_MATCH_IDLE;
                    esp12s_line_start = (receive_data == '\n');
                }
                break;

            case ESP12S_MATCH_ER:
                if(receive_data == 'R')
                    esp12s_match_state = ESP12S_MATCH_ERR;
                else
                {
                    esp12s_match_state = ESP12S_MATCH_IDLE;
                    esp12s_line_start = (receive_data == '\n');
                }
                break;

            case ESP12S_MATCH_ERR:
                if(receive_data == 'O')
                    esp12s_match_state = ESP12S_MATCH_ERRO;
                else
                {
                    esp12s_match_state = ESP12S_MATCH_IDLE;
                    esp12s_line_start = (receive_data == '\n');
                }
                break;

            case ESP12S_MATCH_ERRO:
                if(receive_data == 'R')
                    esp12s_match_state = ESP12S_MATCH_ERROR;
                else
                {
                    esp12s_match_state = ESP12S_MATCH_IDLE;
                    esp12s_line_start = (receive_data == '\n');
                }
                break;

            case ESP12S_MATCH_ERROR:
                if(receive_data == '\r')
                    esp12s_match_state = ESP12S_MATCH_ERROR_CR;
                else
                {
                    esp12s_match_state = ESP12S_MATCH_IDLE;
                    esp12s_line_start = (receive_data == '\n');
                }
                break;

            case ESP12S_MATCH_ERROR_CR:
                if(receive_data == '\n')
                    esp12s_receive_data = ESP12S_RESPONSE_ERROR;

                esp12s_match_state = ESP12S_MATCH_IDLE;
                esp12s_line_start = (receive_data == '\n');
                break;

            default:
                esp12s_match_state = ESP12S_MATCH_IDLE;
                esp12s_line_start = (receive_data == '\n');
                break;
        }
    }
}
