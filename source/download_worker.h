#ifndef DOWNLOAD_WORKER_H
#define DOWNLOAD_WORKER_H

#include <stdbool.h>
#include <stddef.h>

int  download_worker_start(void);
void download_worker_stop(void);
void download_worker_wake(void);
void download_worker_set_foreground(bool active);
void download_worker_notify_suspend(bool suspended);
void download_worker_cancel_current(void);
bool download_worker_is_active(void);
void download_worker_status(char *out, size_t outlen);

#endif
