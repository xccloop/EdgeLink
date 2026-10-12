"""Exercise the BSP IPD parser and APP TCP receiver with host stubs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usart.c"
#include "tcp_receive.c"
static unsigned char incoming;
static unsigned ack_count, query_count;
static uint8_t last_query[2];
static uint32_t last_generation;
int usart_interrupt_flag_get(unsigned port, unsigned flag) { (void)port; (void)flag; return SET; }
unsigned usart_data_receive(unsigned port) { (void)port; return incoming; }
void usart_interrupt_disable(unsigned port, unsigned flag) { (void)port; (void)flag; }
void usart_interrupt_enable(unsigned port, unsigned flag) { (void)port; (void)flag; }
int rtos_tcp_ack_frame_send_from_isr(const tcp_ack_frame_t *f, BaseType_t *w) {
    (void)f; (void)w; ack_count++; return 1;
}
int rtos_tcp_ota_query_send_from_isr(const uint8_t *data, uint32_t generation, BaseType_t *w) {
    (void)w; query_count++; memcpy(last_query, data, 2); last_generation = generation; return 1;
}
static void feed(const char *s) { while(*s) { incoming = (unsigned char)*s++; USART1_IRQHandler(); } }
int main(void) {
    char out[96], small[4], long_line[100]; unsigned i;
    esp12s_response_t response;
    feed("+IPD,2:\x01\x03");
    assert(query_count == 0); /* BSP works without an APP receiver registered. */
    tcp_receive_init();
    feed("AT+CWJAP=\"test\",\"SECRET_SENTINEL\"\r\n");
    assert(esp12s_diagnostic_get(out, sizeof(out)) == 0);
    feed("+CIPSTA:ip:\"192.168.1.114\"\r\n+CIPSTA:gateway:\"192.168.1.1\"\r\n");
    feed("+CIPSTA:netmask:\"255.255.255.0\"\r\n+CIFSR:STAMAC,\"2c:f4:32:15:5f:f5\"\r\nSTATUS:2\r\nOK\r\n");
    assert(esp12s_diagnostic_get(out, sizeof(out)) && strcmp(out, "+CIPSTA:ip:\"192.168.1.114\"") == 0);
    assert(esp12s_diagnostic_get(out, sizeof(out)) && strstr(out, "gateway:"));
    assert(esp12s_diagnostic_get(out, sizeof(out)) && strstr(out, "netmask:"));
    assert(esp12s_diagnostic_get(out, sizeof(out)) && strstr(out, "STAMAC,"));
    assert(esp12s_diagnostic_get(out, sizeof(out)) && strcmp(out, "STATUS:2") == 0);
    assert(esp12s_response_get(&response) && response == ESP12S_RESPONSE_OK);
    for(i = 0; i < 12; i++) feed("STATUS:4\r\n");
    for(i = 0; i < 7; i++) assert(esp12s_diagnostic_get(out, sizeof(out)));
    assert(!esp12s_diagnostic_get(out, sizeof(out)));
    feed("STATUS:2\r\n");
    assert(!esp12s_diagnostic_get(out, 0));
    assert(!esp12s_diagnostic_get(NULL, sizeof(out)));
    assert(esp12s_diagnostic_get(small, sizeof(small)) && strcmp(small, "STA") == 0);
    feed("+CWJAP:\"12345678901234567890123456789012\",\"aa:bb:cc:dd:ee:ff\",11,-80\r\n");
    assert(esp12s_diagnostic_get(out, sizeof(out)) && strstr(out, "aa:bb:cc:dd:ee:ff"));
    memset(long_line, 'x', 95); memcpy(long_line, "+CIPSTATUS:", 11);
    long_line[95] = '\r'; long_line[96] = '\n'; long_line[97] = 0;
    feed(long_line);
    assert(esp12s_diagnostic_get(out, sizeof(out)) && strlen(out) == 95);
    long_line[95] = 'x'; long_line[96] = '\r'; long_line[97] = '\n'; long_line[98] = 0;
    feed(long_line); assert(!esp12s_diagnostic_get(out, sizeof(out)));
    feed("+IPD,26:+CIPSTA:ip:\"192.168.1.114\"\n");
    assert(ack_count == 0 && !esp12s_diagnostic_get(out, sizeof(out)));
    feed("+IPD,1:\x01");
    esp12s_response_reset();
    feed("+IPD,1:\x03");
    assert(query_count == 1 && last_query[0] == 1 && last_query[1] == 3);
    feed("+IPD,2:\x01"); esp12s_response_reset(); feed("\x04");
    assert(query_count == 2 && last_query[1] == 4);
    feed("+IPD,20:EHabcdefghijklmn\x01\x03\x01\x04");
    assert(ack_count == 1 && query_count == 4 && last_query[1] == 4);
    feed("+IPD,1:E+IPD,1:H+IPD,14:abcdefghijklmn");
    assert(ack_count == 2);
    feed("+IPD,1:\x01");
    feed("\r\nCLOSED\r\nCONNECT\r\n");
    esp12s_response_reset();
    feed("+IPD,1:\x03");
    assert(query_count == 4); /* Old connection's partial query was discarded. */
    feed("+IPD,2:\x01\x03");
    assert(query_count == 5 && last_generation == esp12s_tcp_generation_get());
    feed("\r\nERROR\r\n");
    assert(esp12s_response_get(&response) && response == ESP12S_RESPONSE_ERROR);
    puts("ESP diagnostic parser tests passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="edgelink-esp-test-") as temp:
    directory = Path(temp)
    queue = directory / "FreeRtos/Queue/rtos_queue.h"
    queue.parent.mkdir(parents=True)
    queue.write_text("#define TCP_FRAME_LENGTH 16\n#define TCP_OTA_QUERY_TYPE 1\n#define TCP_OTA_QUERY_LENGTH 2\n"
                     "typedef struct { unsigned char data[16]; } tcp_ack_frame_t;\n"
                     "int rtos_tcp_ack_frame_send_from_isr(const tcp_ack_frame_t *, BaseType_t *);\n"
                     "int rtos_tcp_ota_query_send_from_isr(const uint8_t *, uint32_t, BaseType_t *);\n")
    (directory / "FreeRTOS.h").write_text("typedef int BaseType_t;\n#define pdFALSE 0\n#define portYIELD_FROM_ISR(x) ((void)(x))\n")
    (directory / "gd32f10x.h").write_text("#define SET 1\n#define USART0 0\n#define USART1 1\n")
    (directory / "gd32f10x_usart.h").write_text("#define USART_INT_FLAG_RBNE 1\n#define USART_INT_RBNE 1\nint usart_interrupt_flag_get(unsigned,unsigned);\nunsigned usart_data_receive(unsigned);\nvoid usart_interrupt_disable(unsigned,unsigned);\nvoid usart_interrupt_enable(unsigned,unsigned);\n")
    (directory / "test.c").write_text(SOURCE)
    compiler = os.environ.get("HOST_CC", "gcc")
    if not shutil.which(compiler):
        raise SystemExit("Host C compiler missing; set HOST_CC")
    binary = directory / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(directory),
                    "-I" + str(ROOT / "Drivers/BSP/INTERRUPT/USART"),
                    "-I" + str(ROOT / "Drivers/BSP"),
                    "-I" + str(ROOT / "User/App/Protocol/Tcp"),
                    str(directory / "test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
