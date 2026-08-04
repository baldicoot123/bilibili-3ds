#include "download_worker.h"

#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bili.h"
#include "cache_manager.h"
#include "net.h"
#include "thread_util.h"
#include "ui.h"

#define WORKER_STACK (64 * 1024)
#define COPY_BUF_SIZE (64 * 1024)
#define RETRY_MAX 5

static Thread s_thread;
static volatile int s_quit;
static volatile int s_foreground;
static volatile int s_suspended;
static volatile int s_active;
static LightLock s_status_lock;
static char s_status[128];
static bool s_inited;

static void init_once(void) {
	if (s_inited) return;
	LightLock_Init(&s_status_lock);
	s_status[0] = 0;
	s_inited = true;
}

static void set_worker_status(const char *s) {
	LightLock_Lock(&s_status_lock);
	snprintf(s_status, sizeof(s_status), "%s", s ? s : "");
	LightLock_Unlock(&s_status_lock);
}

void download_worker_status(char *out, size_t outlen) {
	if (!out || !outlen) return;
	init_once();
	LightLock_Lock(&s_status_lock);
	snprintf(out, outlen, "%s", s_status);
	LightLock_Unlock(&s_status_lock);
}

static bool interrupted(const DownloadTask *t) {
	return s_quit || s_foreground || s_suspended || net_is_shutting_down() ||
	       !cache_manager_task_is_downloading(t);
}

static uint64_t file_size(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
}

static bool retry_wait(const DownloadTask *t, int attempt) {
	int ms = 500 << attempt;
	if (ms > 3000) ms = 3000;
	for (int elapsed = 0; elapsed < ms; elapsed += 100) {
		if (interrupted(t)) return false;
		svcSleepThread(100 * 1000 * 1000LL);
	}
	return true;
}

/* 0=完成,1=被暂停/抢占,-1=本次尝试失败。 */
static int download_once(DownloadTask *t, unsigned char *buf) {
	char part[CACHE_PATH_MAX];
	cache_manager_part_path(t, part, sizeof(part));
	uint64_t offset = file_size(part);
	if (t->total_size && offset == t->total_size && offset > 0) {
		if (rename(part, t->filepath) == 0) {
			cache_manager_mark_completed(t, offset);
			return 0;
		}
	}

	set_worker_status("正在解析缓存地址");
	char url[2048];
	if (bili_get_play_url(t->bvid, t->cid, t->qn, url, sizeof(url)) != 0)
		return interrupted(t) ? 1 : -1;
	if (interrupted(t)) return 1;

	NetStream ns;
	memset(&ns, 0, sizeof(ns));
	bool restart = false;
	if (ns_open(&ns, url, offset) != 0) {
		/* CDN 不接受较大 Range 时，现有 NetStream 会拒绝假续传。
		 * 先确认从 0 能打开，成功后才截断旧 part，避免网络失败白白丢进度。 */
		if (!offset || interrupted(t) || ns_open(&ns, url, 0) != 0) {
			ns_close(&ns);
			return interrupted(t) ? 1 : -1;
		}
		restart = true;
		offset = 0;
	}
	if (interrupted(t)) { ns_close(&ns); return 1; }
	if (ns.size && offset > ns.size) { ns_close(&ns); return -1; }
	if (offset && t->total_size && ns.size && ns.size != t->total_size) {
		/* URL 刷新后即便 qn 相同，服务端也可能换了媒体表示。总长不同就
		 * 绝不能把新资源追加到旧 part；当前连接已证明网络可用，再从 0
		 * 建立一次，成功后才由下面的 "wb" 截断。 */
		ui_trace("cache representation changed: old=%lu new=%lu",
		         (unsigned long)t->total_size, (unsigned long)ns.size);
		ns_close(&ns);
		if (ns_open(&ns, url, 0) != 0) return interrupted(t) ? 1 : -1;
		restart = true;
		offset = 0;
	}

	FILE *f = fopen(part, restart ? "wb" : "ab");
	if (!f) { ns_close(&ns); return -1; }
	uint64_t done = offset;
	uint64_t total = ns.size ? ns.size : t->total_size;
	t->downloaded_size = done;
	if (total) t->total_size = total;  /* 同一次激活里的后续重试也要看到检查点 */
	cache_manager_update_progress(t, done, total, true);
	uint64_t last_save_bytes = done;
	u64 last_save_ms = osGetTime();
	int result = -1;

	for (;;) {
		if (interrupted(t)) { result = 1; break; }
		long n = ns_read(&ns, buf, COPY_BUF_SIZE);
		if (n < 0) { result = interrupted(t) ? 1 : -1; break; }
		if (n == 0) {
			if (!total || done == total) result = 0;
			break;
		}
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
			ui_trace("cache write failed errno=%d", errno);
			result = -1;
			break;
		}
		done += (uint64_t)n;
		t->downloaded_size = done;
		if (total && done > total) { result = -1; break; }
		char progress[128];
		if (total)
			snprintf(progress, sizeof(progress), "后台下载  %lu%%",
			         (unsigned long)((done * 100u) / total));
		else
			snprintf(progress, sizeof(progress), "后台下载  %luKB",
			         (unsigned long)(done / 1024));
		set_worker_status(progress);
		bool checkpoint = done - last_save_bytes >= 1024 * 1024 ||
		                  osGetTime() - last_save_ms >= 5000;
		cache_manager_update_progress(t, done, total, checkpoint);
		if (checkpoint) { last_save_bytes = done; last_save_ms = osGetTime(); }
	}

	if (fflush(f) != 0 && result == 0) result = -1;
	if (fclose(f) != 0 && result == 0) result = -1;
	ns_close(&ns);
	cache_manager_update_progress(t, done, total, true);
	if (result != 0) return result;
	if (total && done != total) return -1;
	if (rename(part, t->filepath) != 0) return -1;
	cache_manager_mark_completed(t, done);
	set_worker_status("下载完成");
	return 0;
}

static void worker_main(void *unused) {
	(void)unused;
	unsigned char *buf = (unsigned char *)malloc(COPY_BUF_SIZE);
	if (!buf) { set_worker_status("缓存线程内存不足"); return; }
	while (!s_quit && !net_is_shutting_down()) {
		if (s_foreground || s_suspended) {
			set_worker_status(s_foreground ? "在线播放中，缓存已让路" : "缓存已挂起");
			svcSleepThread(100 * 1000 * 1000LL);
			continue;
		}
		DownloadTask task;
		int got = cache_manager_claim_next(&task);
		if (got <= 0) {
			if (got < 0) set_worker_status("缓存数据库写入失败");
			else set_worker_status("");
			svcSleepThread(150 * 1000 * 1000LL);
			continue;
		}
		s_active = 1;
		__dmb();
		int result = -1;
		for (int attempt = 0; attempt < RETRY_MAX; attempt++) {
			result = download_once(&task, buf);
			if (result >= 0) break;
			ui_trace("cache retry %d/%d: %s", attempt + 1, RETRY_MAX, task.bvid);
			if (!retry_wait(&task, attempt)) { result = 1; break; }
		}
		if (result == 1) {
			/* 用户暂停已经由 manager 写成 PAUSED；其余中断恢复为 WAITING。 */
			if (cache_manager_task_is_downloading(&task))
				cache_manager_mark_waiting(&task);
		} else if (result < 0) {
			cache_manager_mark_failed(&task);
			set_worker_status("下载失败，可在离线下载任务中重试");
		}
		s_active = 0;
		__dmb();
	}
	free(buf);
	s_active = 0;
	__dmb();
}

int download_worker_start(void) {
	if (s_thread) return 0;
	init_once();
	s_quit = s_foreground = s_suspended = s_active = 0;
	s_status[0] = 0;
	static const int cores[] = { 3, 2, -2 };
	for (int i = 0; i < 3 && !s_thread; i++)
		s_thread = threadCreate(worker_main, NULL, WORKER_STACK, 0x3A,
		                        cores[i], false);
	return s_thread ? 0 : -1;
}

void download_worker_wake(void) { /* worker 最多 150ms 后轮询到 */ }

void download_worker_set_foreground(bool active) {
	init_once();
	s_foreground = active ? 1 : 0;
	__dmb();
	if (active && s_thread) net_cancel_streams();
}

void download_worker_notify_suspend(bool suspended) {
	init_once();
	s_suspended = suspended ? 1 : 0;
	__dmb();
	if (suspended && s_active && s_thread) net_cancel_streams();
}

void download_worker_cancel_current(void) {
	if (s_active && s_thread) net_cancel_streams();
}

bool download_worker_is_active(void) { return s_active != 0; }

void download_worker_stop(void) {
	if (!s_thread) return;
	s_quit = 1;
	__dmb();
	net_cancel_streams();
	thread_reap(&s_thread, 8000000000ULL, "cache-worker");
}
