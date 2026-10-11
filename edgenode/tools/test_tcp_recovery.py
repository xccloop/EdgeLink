"""Host regression tests for the real tcp.c with mocked UART and time (no devices)."""
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
#include "tcp.c"

volatile uint32_t board_systick_ms;
static esp12s_response_t events[32];
static unsigned head, tail, commands, delays;
static char last_command[128];
static const tcp_config_struct config = {"test", "password", "127.0.0.1", 9527};
void esp12s_init(void) {}
void esp12s_cmd_send(const char *s) { commands++; snprintf(last_command, sizeof(last_command), "%s", s); }
void esp12s_data_send(const uint8_t *data, uint8_t length) { (void)data; (void)length; assert(0); }
void esp12s_response_reset(void) { head = tail = 0; }
uint8_t esp12s_response_get(esp12s_response_t *r) {
    if(head == tail) return 0;
    *r = events[head++]; return 1;
}
uint8_t esp12s_local_ip_get(char *buffer, uint8_t size) { (void)buffer; (void)size; return 0; }
uint8_t esp12s_diagnostic_get(char *buffer, uint8_t size) { (void)buffer; (void)size; return 0; }
void vTaskDelay(unsigned ticks) { delays++; board_systick_ms += ticks; }
static void event(esp12s_response_t r) { assert(tail < 32); events[tail++] = r; }
static void setup(uint32_t now) {
    board_systick_ms = now; tcp_saved_config = &config;
    tcp_connected = TCP_FAIL; recovery_state = TCP_RECOVERY_IDLE;
    recovery_started_ms = now; recovery_wait_ms = 5000;
    recovery_attempts = recovery_ack_misses = 0;
    commands = delays = 0; esp12s_response_reset();
}
static void reconnect(void) {
    board_systick_ms += 5000;
    assert(tcp_recovery_service() == 0);
    assert(strcmp(last_command, ESP_AT_CIPCLOSE) == 0);
    event(ESP12S_RESPONSE_ERROR); /* no old socket */
    tcp_recovery_service();
    assert(recovery_state == TCP_RECOVERY_CONNECT);
    event(ESP12S_RESPONSE_OK); event(ESP12S_RESPONSE_CONNECT);
    assert(tcp_recovery_service() == 1);
    assert(tcp_connected_get() == TCP_SUCCESS);
}
int main(void) {
    unsigned i, n;
    setup(0);
    board_systick_ms = 4999; tcp_recovery_service(); assert(commands == 0);
    board_systick_ms = 5000; tcp_recovery_service(); assert(commands == 1);
    n = commands;
    for(i = 0; i < 1000; i++) tcp_recovery_service();
    assert(commands == n && delays == 0); /* no busy wait */
    assert(tcp_data_send((const uint8_t *)"x", 1) == TCP_SEND_FAIL);
    board_systick_ms += 10000; tcp_recovery_service();
    assert(recovery_state == TCP_RECOVERY_IDLE);
    board_systick_ms += 4999; tcp_recovery_service(); assert(commands == n);
    board_systick_ms++; tcp_recovery_service(); assert(commands == n + 1);

    setup(0);
    for(i = 0; i < 3; i++) {
        unsigned miss;
        reconnect();
        assert(recovery_attempts == i + 1); /* AT success does not heal */
        for(miss = 0; miss < 5; miss++) tcp_recovery_note_ack(0);
        assert(tcp_connected_get() == TCP_FAIL);
    }
    board_systick_ms += 5000; tcp_recovery_service();
    assert(strcmp(last_command, ESP_AT_RST) == 0);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    assert(recovery_state == TCP_RECOVERY_RESET); /* RST OK is not ready */
    event(ESP12S_RESPONSE_READY); tcp_recovery_service();
    assert(recovery_state == TCP_RECOVERY_STATION);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    assert(recovery_state == TCP_RECOVERY_JOIN);
    assert(strncmp(last_command, "AT+CWJAP=", 9) == 0);
    event(ESP12S_RESPONSE_WIFI_CONNECTED); event(ESP12S_RESPONSE_WIFI_GOT_IP);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    assert(strcmp(last_command, "AT+CIPSTA?\r\n") == 0);
    event(ESP12S_RESPONSE_ERROR); tcp_recovery_service(); /* optional on old AT firmware */
    assert(strcmp(last_command, "AT+CIFSR\r\n") == 0);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    assert(strcmp(last_command, "AT+CIPSTATUS\r\n") == 0);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    assert(strcmp(last_command, "AT+CWJAP?\r\n") == 0);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    assert(strcmp(last_command, "AT+SLEEP?\r\n") == 0);
    event(ESP12S_RESPONSE_ERROR); tcp_recovery_service();
    assert(strcmp(last_command, "AT+GMR\r\n") == 0);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    assert(recovery_state == TCP_RECOVERY_MUX);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    assert(recovery_state == TCP_RECOVERY_MODE);
    event(ESP12S_RESPONSE_OK); tcp_recovery_service();
    event(ESP12S_RESPONSE_CONNECT); tcp_recovery_service();
    assert(tcp_connected_get() == TCP_FAIL); /* must also receive OK */
    event(ESP12S_RESPONSE_OK); assert(tcp_recovery_service() == 1);
    tcp_recovery_note_ack(1);
    assert(recovery_attempts == 0 && recovery_ack_misses == 0);
    n = commands; board_systick_ms += 100000; tcp_recovery_service();
    assert(commands == n); /* healthy link is never periodically reset */

    setup(UINT32_MAX - 1000); reconnect(); /* cooldown across clock wrap */
    event(ESP12S_RESPONSE_CLOSED); tcp_recovery_service();
    assert(tcp_connected_get() == TCP_FAIL);
    wifi_connected = TCP_SUCCESS;
    event(ESP12S_RESPONSE_READY); tcp_recovery_service();
    assert(wifi_state_get() == TCP_FAIL);

    setup(0); recovery_attempts = 3;
    board_systick_ms = 5000; tcp_recovery_service();
    board_systick_ms += 15000; tcp_recovery_service();
    n = commands;
    board_systick_ms += 29999; tcp_recovery_service(); assert(commands == n);
    board_systick_ms++; tcp_recovery_service(); assert(commands == n + 1);
    assert(delays == 0);
    puts("TCP recovery regression tests passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="edgelink-tcp-test-") as temp:
    directory = Path(temp)
    (directory / "FreeRTOS.h").write_text("#define pdMS_TO_TICKS(ms) (ms)\n")
    (directory / "task.h").write_text("void vTaskDelay(unsigned ticks);\n")
    (directory / "test.c").write_text(SOURCE)
    compiler = os.environ.get("HOST_CC", "gcc")
    if not shutil.which(compiler):
        raise SystemExit("Host C compiler missing; set HOST_CC")
    binary = directory / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(directory), "-I" + str(ROOT / "User/App/Output/Tcp"),
                    "-I" + str(ROOT / "Drivers/BSP"), "-I" + str(ROOT / "Drivers/BOARD"),
                    str(directory / "test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
