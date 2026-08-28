/**
 * @file rg255_at_rx_probe.c
 * @brief Capture RG255AA raw UART reads and replay at_channel line/classification rules.
 *
 * Test-only diagnostic. It does not change modem configuration.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEVICE_PATH          "/dev/ttyUSB1"
#define COMMAND              "AT+QCFG=\"netmaskset\",3"
#define COMMAND_WIRE         COMMAND "\r\n"
#define EXPECTED_PREFIX      "+QCFG:"
#define RX_BUFFER_SIZE       512U
#define LINE_BUFFER_SIZE     512U
#define RESPONSE_BUFFER_SIZE 4096U
#define CAPTURE_TIMEOUT_MS   5000

typedef struct
{
    char line[LINE_BUFFER_SIZE];
    size_t line_length;
    bool line_discarding;
    char response[RESPONSE_BUFFER_SIZE];
    size_t response_length;
    size_t line_number;
    size_t raw_offset;
    bool transaction_done;
    bool saw_overlong_line;
    bool saw_response_overflow;
} probe_state_t;

static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return -1;
    }

    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static void dump_escaped(const uint8_t *data, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++)
    {
        uint8_t value = data[index];

        if (value == '\r')
        {
            fputs("<CR>", stdout);
        }
        else if (value == '\n')
        {
            fputs("<LF>\n", stdout);
        }
        else if (value == '\0')
        {
            fputs("<NUL>", stdout);
        }
        else if (value >= 0x20U && value <= 0x7eU)
        {
            fputc((int)value, stdout);
        }
        else
        {
            printf("<%02X>", value);
        }
    }

    if (length == 0U || data[length - 1U] != '\n')
    {
        fputc('\n', stdout);
    }
}

static void dump_hex(size_t base_offset, const uint8_t *data, size_t length)
{
    size_t row;

    for (row = 0; row < length; row += 16U)
    {
        size_t column;

        printf("        %04zx :", base_offset + row);
        for (column = 0; column < 16U; column++)
        {
            if (row + column < length)
            {
                printf(" %02X", data[row + column]);
            }
            else
            {
                fputs("   ", stdout);
            }
        }
        fputs("  |", stdout);
        for (column = 0; column < 16U && row + column < length; column++)
        {
            uint8_t value = data[row + column];
            fputc(value >= 0x20U && value <= 0x7eU ? (int)value : '.', stdout);
        }
        fputs("|\n", stdout);
    }
}

static bool has_prefix(const char *line, const char *prefix)
{
    size_t prefix_length = strlen(prefix);
    return strncmp(line, prefix, prefix_length) == 0;
}

static void append_response(probe_state_t *state, const char *line)
{
    size_t line_length = strlen(line);
    size_t separator_length = state->response_length == 0U ? 0U : 1U;

    if (state->response_length + separator_length + line_length >= sizeof(state->response))
    {
        state->saw_response_overflow = true;
        puts("        ROUTE: COMMAND_RESPONSE_APPEND_FAILED (response buffer overflow)");
        return;
    }

    if (separator_length != 0U)
    {
        state->response[state->response_length++] = '\n';
    }
    memcpy(state->response + state->response_length, line, line_length + 1U);
    state->response_length += line_length;
}

static void process_line(probe_state_t *state, const char *line)
{
    size_t length = strlen(line);

    state->line_number++;
    printf("[ASSEMBLED_LINE %zu] len=%zu text=<%s>\n", state->line_number, length, line);
    printf("        transaction=WAITING expected_prefix=<%s> accept_plain_text=false\n", EXPECTED_PREFIX);

    if (strcmp(line, COMMAND) == 0)
    {
        puts("        ROUTE: COMMAND_ECHO_IGNORED");
    }
    else if (strcmp(line, "OK") == 0)
    {
        puts("        ROUTE: SUCCESS_TERMINAL (transaction complete)");
        state->transaction_done = true;
    }
    else if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR:", 11U) == 0 ||
             strncmp(line, "+CMS ERROR:", 11U) == 0)
    {
        puts("        ROUTE: ERROR_TERMINAL + COMMAND_RESPONSE");
        append_response(state, line);
        state->transaction_done = true;
    }
    else if (has_prefix(line, EXPECTED_PREFIX))
    {
        puts("        ROUTE: COMMAND_RESPONSE (expected prefix matched)");
        append_response(state, line);
    }
    else
    {
        puts("        ROUTE: URC_CALLBACK (no expected prefix; plain text disabled)");
    }
}

static void feed_rx(probe_state_t *state, const uint8_t *data, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++)
    {
        uint8_t value = data[index];

        if (value == '\r' || value == '\0')
        {
            continue;
        }

        if (value == '\n')
        {
            if (state->line_discarding)
            {
                state->line_discarding = false;
                state->line_length = 0U;
                continue;
            }

            if (state->line_length == 0U)
            {
                continue;
            }

            state->line[state->line_length] = '\0';
            state->line_length = 0U;
            process_line(state, state->line);
            continue;
        }

        if (state->line_discarding)
        {
            continue;
        }

        if (state->line_length >= sizeof(state->line) - 1U)
        {
            state->saw_overlong_line = true;
            state->line_discarding = true;
            state->line_length = 0U;
            puts("[LINE_BUFFER] OVERFLOW: current physical line discarded");
            continue;
        }

        state->line[state->line_length++] = (char)value;
    }
}

static int configure_serial(int fd, struct termios *original)
{
    struct termios tty;

    if (tcgetattr(fd, original) != 0)
    {
        return -errno;
    }

    tty = *original;
    cfmakeraw(&tty);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    tty.c_cflag |= CS8;
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif
    if (cfsetispeed(&tty, B115200) != 0 || cfsetospeed(&tty, B115200) != 0)
    {
        return -errno;
    }
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        return -errno;
    }

    if (tcflush(fd, TCIOFLUSH) != 0)
    {
        return -errno;
    }

    return 0;
}

static int write_all(int fd, const uint8_t *data, size_t length)
{
    size_t written = 0U;

    while (written < length)
    {
        ssize_t result = write(fd, data + written, length - written);

        if (result > 0)
        {
            written += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR)
        {
            continue;
        }
        return result < 0 ? -errno : -EIO;
    }

    return 0;
}

int main(void)
{
    uint8_t rx[RX_BUFFER_SIZE];
    struct termios original;
    probe_state_t state;
    long long deadline;
    size_t read_number = 0U;
    int fd;
    int ret;

    memset(&state, 0, sizeof(state));
    printf("[CONFIG] device=%s command=<%s> TX_suffix=<CR><LF>\n", DEVICE_PATH, COMMAND);
    printf("[CONFIG] expected_prefix=<%s> accept_plain_text=false\n", EXPECTED_PREFIX);
    printf("[BUFFERS] uart_read=%u line=%u (max payload=%u) response=%u\n",
           RX_BUFFER_SIZE, LINE_BUFFER_SIZE, LINE_BUFFER_SIZE - 1U, RESPONSE_BUFFER_SIZE);

    fd = open(DEVICE_PATH, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "open %s failed: %s\n", DEVICE_PATH, strerror(errno));
        return 1;
    }

    if (ioctl(fd, TIOCEXCL) != 0)
    {
        fprintf(stderr, "TIOCEXCL failed (device may be busy): %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    ret = configure_serial(fd, &original);
    if (ret != 0)
    {
        fprintf(stderr, "configure serial failed: %s\n", strerror(-ret));
        close(fd);
        return 1;
    }

    printf("[TX] len=%zu escaped=", strlen(COMMAND_WIRE));
    dump_escaped((const uint8_t *)COMMAND_WIRE, strlen(COMMAND_WIRE));
    ret = write_all(fd, (const uint8_t *)COMMAND_WIRE, strlen(COMMAND_WIRE));
    if (ret != 0)
    {
        fprintf(stderr, "write failed: %s\n", strerror(-ret));
        (void)tcsetattr(fd, TCSANOW, &original);
        close(fd);
        return 1;
    }

    deadline = monotonic_ms() + CAPTURE_TIMEOUT_MS;
    while (!state.transaction_done && monotonic_ms() < deadline)
    {
        struct pollfd descriptor;

        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        ret = poll(&descriptor, 1, 250);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            break;
        }
        if (ret == 0 || (descriptor.revents & POLLIN) == 0)
        {
            continue;
        }

        {
            ssize_t length = read(fd, rx, sizeof(rx));
            if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            {
                continue;
            }
            if (length <= 0)
            {
                fprintf(stderr, "read returned %zd: %s\n", length,
                        length < 0 ? strerror(errno) : "EOF");
                break;
            }

            read_number++;
            printf("[UART_READ %zu] len=%zd raw_offset=%zu\n", read_number, length, state.raw_offset);
            dump_hex(state.raw_offset, rx, (size_t)length);
            fputs("        ESCAPED: ", stdout);
            dump_escaped(rx, (size_t)length);
            feed_rx(&state, rx, (size_t)length);
            state.raw_offset += (size_t)length;
        }
    }

    if (state.line_length != 0U)
    {
        printf("[INCOMPLETE_LINE] len=%zu (no LF received)\n", state.line_length);
    }

    printf("[SUMMARY] reads=%zu raw_bytes=%zu assembled_lines=%zu transaction_done=%s\n",
           read_number, state.raw_offset, state.line_number,
           state.transaction_done ? "true" : "false");
    printf("[SUMMARY] command_response_len=%zu text=<%s>\n",
           state.response_length, state.response);
    printf("[SUMMARY] line_overflow=%s response_overflow=%s\n",
           state.saw_overlong_line ? "true" : "false",
           state.saw_response_overflow ? "true" : "false");

    (void)tcsetattr(fd, TCSANOW, &original);
    close(fd);
    return state.transaction_done ? 0 : 2;
}
