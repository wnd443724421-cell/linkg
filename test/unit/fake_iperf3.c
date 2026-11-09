/**
 * @file fake_iperf3.c
 * @brief Web测速子进程生命周期测试夹具，不进行网络传输
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-09
 */

#define _POSIX_C_SOURCE 200809L

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief 输出可控的iperf3格式结果或保持子进程存活。
 */
int main(int argc, char **argv)
{
    const char *mode = getenv("LINKG_TEST_IPERF_MODE");
    int         port = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int index = 1; index + 1 < argc; index++)
    {
        if (strcmp(argv[index], "-p") == 0)
        {
            port = atoi(argv[index + 1]);
        }
    }
    if (mode != NULL && strcmp(mode, "busy") == 0 && port < 5203)
    {
        puts("iperf3: error - the server is busy running a test. try again later");
        return 1;
    }
    if (mode != NULL && strcmp(mode, "stubborn") == 0)
    {
        signal(SIGTERM, SIG_IGN);
        puts("connected");
        poll(NULL, 0, 10000);
        return 0;
    }
    if (mode != NULL && strcmp(mode, "missing_summary") == 0)
    {
        puts("[  5] 0.00-1.00 sec 1 MBytes 8.00 Mbits/sec");
        return 0;
    }
    if (mode != NULL && strcmp(mode, "exit_failure") == 0)
    {
        return 2;
    }
    if (mode != NULL && strcmp(mode, "long_eof") == 0)
    {
        for (int index = 0; index < 1024; index++)
        {
            putchar('x');
        }
        return 0;
    }

    puts("[  5] 0.00-1.00 sec 1 MBytes 8.00 Mbits/sec");
    puts("[  5] 1.00-2.00 sec 1 MBytes 8.00 Mbits/sec");
    puts("[  5] 0.00-5.00 sec 5 MBytes 8.00 Mbits/sec sender");
    puts("[  5] 0.00-5.00 sec 4 MBytes 7.50 Mbits/sec 0.100 ms 1/100 (1%) receiver");

    if (mode != NULL && strcmp(mode, "hold_pipe") == 0)
    {
        pid_t holder = fork();
        if (holder < 0)
        {
            return 2;
        }
        if (holder == 0)
        {
            poll(NULL, 0, 1200);
            _exit(0);
        }
    }
    return 0;
}
