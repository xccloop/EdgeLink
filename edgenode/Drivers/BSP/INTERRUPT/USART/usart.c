#include "usart.h"
#include "gd32f10x.h"
#include "gd32f10x_usart.h"

/* USART0连接CH340，USART1连接ESP12S；中断都只取走已经到达的字节，不等待也不发送AT命令。 */
volatile uint8_t ch340_receive_data = 0U;

void USART0_IRQHandler(void)
{
    if(usart_interrupt_flag_get(USART0, USART_INT_FLAG_RBNE) == SET)
    {
        ch340_receive_data = (uint8_t)usart_data_receive(USART0);
    }
}

/*
    ESP可能连续返回CONNECT\r\nOK\r\n。若只保存“最近一次回应”，主循环读取CONNECT后，
    中断可能立刻写入OK，再被主循环清掉。因此这里使用小型环形队列：
    USART1中断只负责放入事件，APP主循环只负责按顺序取出事件。
*/
#define ESP12S_RESPONSE_QUEUE_SIZE  8U
#define ESP12S_RESPONSE_LINE_SIZE   32U

static volatile esp12s_response_t esp12s_response_queue[ESP12S_RESPONSE_QUEUE_SIZE];
static volatile uint8_t esp12s_response_write_index;
static volatile uint8_t esp12s_response_read_index;
static char esp12s_response_line[ESP12S_RESPONSE_LINE_SIZE];
static uint8_t esp12s_response_line_length;
static uint8_t esp12s_response_line_overflow;

static void esp12s_response_push(esp12s_response_t response)
{
    uint8_t next_index = (uint8_t)(esp12s_response_write_index + 1U);

    if(next_index >= ESP12S_RESPONSE_QUEUE_SIZE)
    {
        next_index = 0U;
    }

    /* 队列满时保留已经收到的旧事件，丢弃最新事件；初始化期间正常不会积压到8个。 */
    if(next_index != esp12s_response_read_index)
    {
        esp12s_response_queue[esp12s_response_write_index] = response;
        esp12s_response_write_index = next_index;
    }
}

static uint8_t esp12s_response_line_equal(const char *text)
{
    uint8_t i = 0U;

    while(text[i] != '\0')
    {
        if((i >= esp12s_response_line_length) || (esp12s_response_line[i] != text[i]))
        {
            return 0U;
        }
        i++;
    }

    return (i == esp12s_response_line_length) ? 1U : 0U;
}

static uint8_t esp12s_response_line_start_with(const char *text)
{
    uint8_t i = 0U;

    while(text[i] != '\0')
    {
        if((i >= esp12s_response_line_length) || (esp12s_response_line[i] != text[i]))
        {
            return 0U;
        }
        i++;
    }

    return 1U;
}

/*
    每收到一行完整AT文本，就翻译成一个事件放入队列。
    +IPD后面跟着真实TCP负载，后续需要专门的长度解析和接收缓冲区；本阶段不把它误当AT回应。
*/
static void esp12s_response_line_handle(void)
{
    if(esp12s_response_line_equal("OK") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_OK);
    }
    else if(esp12s_response_line_equal("ERROR") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_ERROR);
    }
    else if(esp12s_response_line_equal("FAIL") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_FAIL);
    }
    else if(esp12s_response_line_equal("SEND OK") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_SEND_OK);
    }
    else if(esp12s_response_line_equal("CONNECT") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_CONNECT);
    }
    else if(esp12s_response_line_equal("CLOSED") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_CLOSED);
    }
    else if(esp12s_response_line_equal("WIFI CONNECTED") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_WIFI_CONNECTED);
    }
    else if(esp12s_response_line_equal("WIFI GOT IP") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_WIFI_GOT_IP);
    }
    else if(esp12s_response_line_equal("ready") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_READY);
    }
    else if(esp12s_response_line_start_with("busy") != 0U)
    {
        esp12s_response_push(ESP12S_RESPONSE_BUSY);
    }
}

uint8_t esp12s_response_get(esp12s_response_t *response)
{
    uint8_t next_index;

    if((response == 0) || (esp12s_response_read_index == esp12s_response_write_index))
    {
        return 0U;
    }

    *response = esp12s_response_queue[esp12s_response_read_index];
    next_index = (uint8_t)(esp12s_response_read_index + 1U);
    esp12s_response_read_index = (next_index >= ESP12S_RESPONSE_QUEUE_SIZE) ? 0U : next_index;
    return 1U;
}

void esp12s_response_reset(void)
{
    /* 复位队列和半行状态时暂时关闭RX中断，避免中断写指针与APP同时修改。 */
    usart_interrupt_disable(USART1, USART_INT_RBNE);
    esp12s_response_write_index = 0U;
    esp12s_response_read_index = 0U;
    esp12s_response_line_length = 0U;
    esp12s_response_line_overflow = 0U;
    usart_interrupt_enable(USART1, USART_INT_RBNE);
}

void USART1_IRQHandler(void)
{
    uint8_t receive_data;

    if(usart_interrupt_flag_get(USART1, USART_INT_FLAG_RBNE) == SET)
    {
        receive_data = (uint8_t)usart_data_receive(USART1);

        /* CIPSEND的准备完成提示是单个'>', 不一定按照普通文本行结束。 */
        if(receive_data == '>')
        {
            esp12s_response_push(ESP12S_RESPONSE_PROMPT);
            return;
        }

        if(receive_data == '\r')
        {
            return;
        }

        if(receive_data == '\n')
        {
            if((esp12s_response_line_length != 0U) && (esp12s_response_line_overflow == 0U))
            {
                esp12s_response_line_handle();
            }

            esp12s_response_line_length = 0U;
            esp12s_response_line_overflow = 0U;
            return;
        }

        if(esp12s_response_line_length < (ESP12S_RESPONSE_LINE_SIZE - 1U))
        {
            esp12s_response_line[esp12s_response_line_length] = (char)receive_data;
            esp12s_response_line_length++;
        }
        else
        {
            /* 超长行不是本初始化状态机需要的AT回应，丢到换行后再重新同步。 */
            esp12s_response_line_overflow = 1U;
        }
    }
}
