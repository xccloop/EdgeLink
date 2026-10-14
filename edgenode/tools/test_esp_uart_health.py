"""Exercise actual USART ISR counters without exposing received payloads."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include "usart.c"
static uint8_t incoming;
static int frame_error, overrun;
int usart_interrupt_flag_get(unsigned port, unsigned flag) { (void)port; (void)flag; return SET; }
int usart_flag_get(unsigned port, unsigned flag) {
    (void)port; return flag == USART_FLAG_FERR ? frame_error : overrun;
}
unsigned usart_data_receive(unsigned port) { (void)port; return incoming; }
void usart_interrupt_disable(unsigned port, unsigned flag) { (void)port; (void)flag; }
void usart_interrupt_enable(unsigned port, unsigned flag) { (void)port; (void)flag; }
static void feed(const char *text) {
    while(*text) { incoming = (uint8_t)*text++; USART1_IRQHandler(); }
}
int main(void) {
    esp12s_uart_health_t before, after;
    esp12s_response_t event;
    unsigned i;
    esp12s_uart_health_get(&before);
    assert(before.rx_bytes == 0U && before.events == 0U);
    feed("OK\r\n"); esp12s_uart_health_get(&after);
    assert(after.rx_bytes == 4U && after.text_lines == 1U && after.events == 1U);
    assert(esp12s_response_get(&event) && event == ESP12S_RESPONSE_OK);
    before = after;
    frame_error = overrun = 1; feed("x"); frame_error = overrun = 0;
    esp12s_uart_health_get(&after);
    assert(after.rx_bytes == before.rx_bytes + 1U && after.partial_line_length == 1U);
    assert(after.framing_errors == 1U && after.overrun_errors == 1U);
    feed("\n");
    for(i = 0; i < 10U; ++i) feed("OK\r\n");
    esp12s_uart_health_get(&after);
    assert(after.events == 11U && after.dropped_events == 3U);
    esp12s_response_reset();
    feed("+IPD,5:ab"); esp12s_uart_health_get(&after);
    assert(after.ipd_remaining == 3U);
    before = after; feed("OK\n"); esp12s_uart_health_get(&after);
    assert(after.events == before.events && after.ipd_remaining == 0U);
    feed("OK\r\n"); esp12s_uart_health_get(&after);
    assert(after.events == before.events + 1U);
    puts("ESP UART health distinguishes no input, parser backlog, errors and dropped events");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='edgelink-uart-health-') as temp:
    directory = Path(temp)
    (directory/'gd32f10x.h').write_text('#define SET 1\n#define USART0 0\n#define USART1 1\n')
    (directory/'gd32f10x_usart.h').write_text('''#define USART_INT_FLAG_RBNE 1
#define USART_INT_RBNE 1
#define USART_FLAG_FERR 2
#define USART_FLAG_ORERR 3
int usart_interrupt_flag_get(unsigned,unsigned);
int usart_flag_get(unsigned,unsigned);
unsigned usart_data_receive(unsigned);
void usart_interrupt_disable(unsigned,unsigned);
void usart_interrupt_enable(unsigned,unsigned);
''')
    (directory/'test.c').write_text(SOURCE)
    binary = directory/('test.exe' if os.name=='nt' else 'test')
    subprocess.run([os.environ.get('HOST_CC','gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(directory), '-I'+str(ROOT/'Drivers/BSP/INTERRUPT/USART'),
                    str(directory/'test.c'), '-o', str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
