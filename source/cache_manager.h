#ifndef CACHE_MANAGER_H
#define CACHE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CACHE_MAX_TASKS 64
#define CACHE_PATH_MAX  512

typedef enum {
	DOWNLOAD_STATUS_WAITING = 0,
	DOWNLOAD_STATUS_DOWNLOADING,
	DOWNLOAD_STATUS_PAUSED,
	DOWNLOAD_STATUS_FAILED,
	DOWNLOAD_STATUS_COMPLETED
} DownloadStatus;

typedef struct {
	char bvid[16];
	int64_t cid;
	int64_t aid;
	char title[200];
	char author[64];
	int qn;
	DownloadStatus status;
	uint64_t downloaded_size;
	uint64_t total_size;
	char filepath[CACHE_PATH_MAX];
	int64_t created_time;
} DownloadTask;

int  cache_manager_init(void);
void cache_manager_shutdown(void);

int cache_manager_enqueue(const char *bvid, int64_t cid, int64_t aid,
                          const char *title, const char *author, int qn);
int cache_manager_count(void);
int cache_manager_snapshot(DownloadTask *out, int max);

int cache_manager_pause(const DownloadTask *task);
int cache_manager_resume(const DownloadTask *task);
int cache_manager_retry(const DownloadTask *task);
int cache_manager_remove(const DownloadTask *task);

/* download_worker 专用。所有接口都按稳定键 (bvid,cid,qn) 查找，调用方
 * 永远不保存管理器内部指针。 */
int  cache_manager_claim_next(DownloadTask *out);
bool cache_manager_task_is_downloading(const DownloadTask *task);
int  cache_manager_update_progress(const DownloadTask *task,
                                   uint64_t downloaded, uint64_t total,
                                   bool force_save);
int  cache_manager_mark_waiting(const DownloadTask *task);
int  cache_manager_mark_failed(const DownloadTask *task);
int  cache_manager_mark_completed(const DownloadTask *task,
                                  uint64_t final_size);

const char *cache_manager_status_name(DownloadStatus status);
void cache_manager_part_path(const DownloadTask *task, char *out, size_t outlen);

#endif
