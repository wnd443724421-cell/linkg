#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uart_diagnostic_internal.h"
#include "linkg_log.h"
#include "linkg_uart.h"

static const char *console_text;
static int open_calls;
static bool proc_error;

FILE *__wrap_fopen(const char *path, const char *mode)
{
    (void)mode;
    assert(strcmp(path, "/proc/consoles") == 0);
    if (proc_error) { errno = EACCES; return NULL; }
    FILE *file = tmpfile();
    assert(file != NULL);
    fputs(console_text, file);
    rewind(file);
    return file;
}
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    (void)level; (void)file; (void)line; (void)format;
}
uint64_t linkg_time_elapsed_ms(void) { return 0; }
int linkg_time_sleep_us(uint32_t us) { (void)us; return 0; }
bool uart_diagnostic_cancel_requested(void) { return false; }
const char *linkg_uart_manager_device_path(uint8_t uart) { (void)uart; return "/dev/ttyS3"; }
int linux_uart_open(const char *path, const uart_config_t *config)
{
    (void)path; (void)config; open_calls++; return -ENODEV;
}
int linux_uart_read(int fd, void *data, int len) { (void)fd; (void)data; (void)len; return -EIO; }
int linux_uart_write(int fd, const void *data, int len) { (void)fd; (void)data; (void)len; return -EIO; }
int linux_uart_close(int fd) { (void)fd; return 0; }
void linux_uart_flush(int fd) { (void)fd; }

int main(void)
{
    linkg_uart_diagnostic_test_result_t result;
    console_text = "ttyS3                -W- (EC p a) 248:3\n";
    assert(uart_diagnostic_test_port(1, LINKG_UART_DIAGNOSTIC_PATH_NORMAL, &result) == -EBUSY);
    assert(open_calls == 0 && result.tx_bytes == 0 && result.error == -EBUSY);
    assert(result.state == LINKG_UART_DIAGNOSTIC_TEST_FAILED);
    console_text = "ttyS0                -W- (EC p a) 248:0\n";
    assert(uart_diagnostic_test_port(1, LINKG_UART_DIAGNOSTIC_PATH_NORMAL, &result) == -ENODEV);
    assert(open_calls == 1);
    console_text = "ttyS3                -W- ( C p a) 248:3\n";
    assert(uart_diagnostic_test_port(1, LINKG_UART_DIAGNOSTIC_PATH_NORMAL, &result) == -ENODEV);
    assert(open_calls == 2);
    proc_error = true;
    assert(uart_diagnostic_test_port(1, LINKG_UART_DIAGNOSTIC_PATH_NORMAL, &result) == -EACCES);
    assert(open_calls == 2);
    puts("PASS console conflict rejected before opening/transmitting, other/disabled console, proc failure");
    return 0;
}
