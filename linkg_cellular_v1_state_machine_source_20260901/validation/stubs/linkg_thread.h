#ifndef LINKG_THREAD_H
#define LINKG_THREAD_H
#include <stdbool.h>
typedef struct linkg_thread
{
    int dummy;
} linkg_thread_t;
bool linkg_thread_is_running(const linkg_thread_t *thread);
int  linkg_thread_get_wakeup_fd(const linkg_thread_t *thread);
int  linkg_thread_clear_wakeup(linkg_thread_t *thread);
#endif
