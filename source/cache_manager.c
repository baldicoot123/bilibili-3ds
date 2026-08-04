#include "cache_manager.h"

#include <3ds.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define CACHE_ROOT      "sdmc:/3ds/3danmu/cache"
#define CACHE_VIDEO_DIR CACHE_ROOT "/videos"
#define CACHE_DB_DIR    CACHE_ROOT "/database"
#define CACHE_DB        CACHE_DB_DIR "/tasks.db"
#define CACHE_DB_TMP    CACHE_DB_DIR "/tasks.db.tmp"
#define CACHE_DB_BAK    CACHE_DB_DIR "/tasks.db.bak"
#define CACHE_DB_VERSION 1u

static DownloadTask s_tasks[CACHE_MAX_TASKS];
static int s_count;
static uint32_t s_generation;
static LightLock s_lock;
static bool s_ready;

typedef struct __attribute__((packed)) {
	char magic[8];
	uint32_t version;
	uint32_t count;
	uint32_t generation;
	uint32_t checksum;
} DbHeader;

typedef struct __attribute__((packed)) {
	char bvid[16];
	int64_t cid;
	int64_t aid;
	char title[200];
	char author[64];
	int32_t qn;
	int32_t status;
	uint64_t downloaded_size;
	uint64_t total_size;
	char filepath[CACHE_PATH_MAX];
	int64_t created_time;
} DbTask;

static uint32_t fnv1a(uint32_t h, const void *data, size_t len) {
	const unsigned char *p = (const unsigned char *)data;
	while (len--) { h ^= *p++; h *= 16777619u; }
	return h;
}

static bool ensure_dir(const char *path) {
	if (mkdir(path, 0777) == 0) return true;
	struct stat st;
	return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool make_dirs(void) {
	return ensure_dir("sdmc:/3ds") &&
	       ensure_dir("sdmc:/3ds/3danmu") &&
	       ensure_dir(CACHE_ROOT) && ensure_dir(CACHE_VIDEO_DIR) &&
	       ensure_dir(CACHE_DB_DIR);
}

static int utf8_len(unsigned char c) {
	if (c < 0x80) return 1;
	if ((c & 0xE0) == 0xC0) return 2;
	if ((c & 0xF0) == 0xE0) return 3;
	if ((c & 0xF8) == 0xF0) return 4;
	return 0;
}

static bool reserved_name(const char *s) {
	char up[8] = {0};
	/* FAT/Windows 把 CON.txt、COM1.mp4 也视为保留名，判断的是第一个点
	 * 之前的主文件名，而不只是完整组件。 */
	size_t n = strcspn(s, ".");
	if (n >= sizeof(up)) return false;
	for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)s[i]);
	if (!strcmp(up, "CON") || !strcmp(up, "PRN") || !strcmp(up, "AUX") ||
	    !strcmp(up, "NUL")) return true;
	if (n == 4 && (!strncmp(up, "COM", 3) || !strncmp(up, "LPT", 3)) &&
	    up[3] >= '1' && up[3] <= '9') return true;
	return false;
}

/* 保留 UTF-8 标题，只清理 FAT 路径中不合法/危险的 ASCII 字符。 */
static void safe_component(const char *src, char *dst, size_t cap,
                           const char *fallback) {
	if (!cap) return;
	size_t o = 0;
	const unsigned char *p = (const unsigned char *)(src ? src : "");
	while (*p && o + 1 < cap) {
		if (*p < 0x80) {
			unsigned char c = *p++;
			if (c < 32 || strchr("<>:\"/\\|?*", c)) c = '_';
			if (o + 1 >= cap) break;
			dst[o++] = (char)c;
			continue;
		}
		int n = utf8_len(*p);
		if (!n) { dst[o++] = '_'; p++; continue; }
		bool valid = true;
		for (int i = 1; i < n; i++)
			if (!p[i] || (p[i] & 0xC0) != 0x80) valid = false;
		if (!valid) { dst[o++] = '_'; p++; continue; }
		if (o + (size_t)n >= cap) break;
		for (int i = 0; i < n; i++) dst[o++] = (char)*p++;
	}
	while (o && (dst[o - 1] == ' ' || dst[o - 1] == '.')) dst[--o] = 0;
	dst[o] = 0;
	if (!dst[0] || !strcmp(dst, ".") || !strcmp(dst, ".."))
		snprintf(dst, cap, "%s", fallback);
	if (reserved_name(dst)) {
		char tmp[160];
		snprintf(tmp, sizeof(tmp), "_%s", dst);
		snprintf(dst, cap, "%s", tmp);
	}
}

static bool key_eq(const DownloadTask *a, const DownloadTask *b) {
	return a->cid == b->cid && a->qn == b->qn && !strcmp(a->bvid, b->bvid);
}

static bool valid_bvid(const char *s) {
	if (!s || !s[0] || strlen(s) >= sizeof(((DownloadTask *)0)->bvid)) return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s)) return false;
	return true;
}

static int find_locked(const DownloadTask *key) {
	for (int i = 0; i < s_count; i++) if (key_eq(&s_tasks[i], key)) return i;
	return -1;
}

static int find_media_locked(const char *bvid, int64_t cid) {
	for (int i = 0; i < s_count; i++)
		if (s_tasks[i].cid == cid && !strcmp(s_tasks[i].bvid, bvid)) return i;
	return -1;
}

static bool file_exists(const char *path, uint64_t *size) {
	struct stat st;
	if (stat(path, &st) != 0) return false;
	if (size) *size = (uint64_t)st.st_size;
	return S_ISREG(st.st_mode);
}

static void u64_dec(uint64_t v, char *out, size_t n) {
	char rev[24]; int k = 0;
	do { rev[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < (int)sizeof(rev));
	size_t o = 0;
	while (k && o + 1 < n) out[o++] = rev[--k];
	if (n) out[o] = 0;
}

void cache_manager_part_path(const DownloadTask *task, char *out, size_t outlen) {
	if (!outlen) return;
	snprintf(out, outlen, "%s.part", task ? task->filepath : "");
}

static void to_disk(const DownloadTask *s, DbTask *d) {
	memset(d, 0, sizeof(*d));
	memcpy(d->bvid, s->bvid, sizeof(d->bvid));
	d->cid = s->cid; d->aid = s->aid;
	memcpy(d->title, s->title, sizeof(d->title));
	memcpy(d->author, s->author, sizeof(d->author));
	d->qn = s->qn; d->status = (int32_t)s->status;
	d->downloaded_size = s->downloaded_size;
	d->total_size = s->total_size;
	memcpy(d->filepath, s->filepath, sizeof(d->filepath));
	d->created_time = s->created_time;
}

static void from_disk(const DbTask *d, DownloadTask *s) {
	memset(s, 0, sizeof(*s));
	memcpy(s->bvid, d->bvid, sizeof(s->bvid)); s->bvid[15] = 0;
	s->cid = d->cid; s->aid = d->aid;
	memcpy(s->title, d->title, sizeof(s->title)); s->title[199] = 0;
	memcpy(s->author, d->author, sizeof(s->author)); s->author[63] = 0;
	s->qn = d->qn; s->status = (DownloadStatus)d->status;
	s->downloaded_size = d->downloaded_size;
	s->total_size = d->total_size;
	memcpy(s->filepath, d->filepath, sizeof(s->filepath));
	s->filepath[CACHE_PATH_MAX - 1] = 0;
	s->created_time = d->created_time;
}

static int save_locked(void) {
	DbHeader h = {{'3','D','M','C','Q','0','1','\0'}, CACHE_DB_VERSION,
	              (uint32_t)s_count, s_generation + 1, 2166136261u};
	DbTask d;
	for (int i = 0; i < s_count; i++) {
		to_disk(&s_tasks[i], &d);
		h.checksum = fnv1a(h.checksum, &d, sizeof(d));
	}
	FILE *f = fopen(CACHE_DB_TMP, "wb");
	if (!f) return -1;
	bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h);
	for (int i = 0; ok && i < s_count; i++) {
		to_disk(&s_tasks[i], &d);
		ok = fwrite(&d, 1, sizeof(d), f) == sizeof(d);
	}
	if (fflush(f) != 0) ok = false;
	if (fclose(f) != 0) ok = false;
	if (!ok) return -2;
	remove(CACHE_DB_BAK);
	rename(CACHE_DB, CACHE_DB_BAK);
	if (rename(CACHE_DB_TMP, CACHE_DB) != 0) {
		rename(CACHE_DB_BAK, CACHE_DB);
		return -3;
	}
	s_generation = h.generation;
	return 0;
}

static int load_file(const char *path, DownloadTask *out, int *count,
                     uint32_t *generation) {
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	DbHeader h;
	if (fread(&h, 1, sizeof(h), f) != sizeof(h) ||
	    memcmp(h.magic, "3DMCQ01", 7) || h.version != CACHE_DB_VERSION ||
	    h.count > CACHE_MAX_TASKS) { fclose(f); return -2; }
	uint32_t sum = 2166136261u;
	for (uint32_t i = 0; i < h.count; i++) {
		DbTask d;
		if (fread(&d, 1, sizeof(d), f) != sizeof(d)) { fclose(f); return -3; }
		sum = fnv1a(sum, &d, sizeof(d));
		from_disk(&d, &out[i]);
		if (!valid_bvid(out[i].bvid) || out[i].cid <= 0 ||
		    (out[i].qn != 16 && out[i].qn != 32) ||
		    out[i].status < DOWNLOAD_STATUS_WAITING ||
		    out[i].status > DOWNLOAD_STATUS_COMPLETED) {
			fclose(f); return -4;
		}
	}
	int extra = fgetc(f);
	fclose(f);
	if (sum != h.checksum || extra != EOF) return -5;
	*count = (int)h.count; *generation = h.generation;
	return 0;
}

static void canonicalize_loaded(DownloadTask *t) {
	char author[80], title[192], want[CACHE_PATH_MAX], collision_path[CACHE_PATH_MAX];
	safe_component(t->author, author, sizeof(author), "未知UP主");
	safe_component(t->title, title, sizeof(title), "未命名视频");
	snprintf(want, sizeof(want), "%s/%s/%s.mp4", CACHE_VIDEO_DIR, author, title);
	char cidbuf[24];
	u64_dec((uint64_t)t->cid, cidbuf, sizeof(cidbuf));
	snprintf(collision_path, sizeof(collision_path), "%s/%s/%s [%s-%s-%d].mp4",
	         CACHE_VIDEO_DIR, author, title, t->bvid, cidbuf, t->qn);
	/* 旧数据库或损坏路径不能决定我们会读写/删除哪里。只有规范根目录下的
	 * 路径可保留；否则按元数据重建。 */
	bool exact = !strcmp(t->filepath, want);
	bool collision = !strcmp(t->filepath, collision_path);
	if ((!exact && !collision) || strstr(t->filepath, "..") ||
	    strchr(t->filepath + 5, ':'))
		snprintf(t->filepath, sizeof(t->filepath), "%s", want);
	ensure_dir(CACHE_VIDEO_DIR);
	char dir[CACHE_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/%s", CACHE_VIDEO_DIR, author);
	ensure_dir(dir);
}

int cache_manager_init(void) {
	LightLock_Init(&s_lock);
	if (!make_dirs()) return -1;
	DownloadTask *best = (DownloadTask *)calloc(CACHE_MAX_TASKS, sizeof(*best));
	DownloadTask *tmp = (DownloadTask *)calloc(CACHE_MAX_TASKS, sizeof(*tmp));
	if (!best || !tmp) { free(best); free(tmp); return -1; }
	int best_n = 0; uint32_t best_g = 0; bool have = false;
	const char *paths[] = { CACHE_DB, CACHE_DB_TMP, CACHE_DB_BAK };
	for (int p = 0; p < 3; p++) {
		int n = 0; uint32_t g = 0;
		if (load_file(paths[p], tmp, &n, &g) == 0 && (!have || g > best_g)) {
			memcpy(best, tmp, (size_t)n * sizeof(*best));
			best_n = n; best_g = g; have = true;
		}
	}
	s_count = best_n; s_generation = best_g;
	memcpy(s_tasks, best, (size_t)s_count * sizeof(*best));
	free(best); free(tmp);
	for (int i = 0; i < s_count; i++) {
		DownloadTask *t = &s_tasks[i];
		canonicalize_loaded(t);
		char part[CACHE_PATH_MAX]; uint64_t fs = 0, ps = 0;
		cache_manager_part_path(t, part, sizeof(part));
		bool final_ok = file_exists(t->filepath, &fs);
		bool part_ok = file_exists(part, &ps);
		if (final_ok) {
			t->downloaded_size = fs;
			if (!t->total_size || fs == t->total_size) {
				t->total_size = fs; t->status = DOWNLOAD_STATUS_COMPLETED;
			} else t->status = DOWNLOAD_STATUS_FAILED;
		} else if (part_ok) {
			t->downloaded_size = ps;
			if (t->total_size && ps > t->total_size)
				t->status = DOWNLOAD_STATUS_FAILED;
			else if (t->status == DOWNLOAD_STATUS_DOWNLOADING ||
			         t->status == DOWNLOAD_STATUS_COMPLETED)
				t->status = DOWNLOAD_STATUS_WAITING;
		} else {
			t->downloaded_size = 0;
			if (t->status == DOWNLOAD_STATUS_COMPLETED)
				t->status = DOWNLOAD_STATUS_FAILED;
			else if (t->status == DOWNLOAD_STATUS_DOWNLOADING)
				t->status = DOWNLOAD_STATUS_WAITING;
		}
	}
	s_ready = true;
	LightLock_Lock(&s_lock);
	int r = save_locked();
	LightLock_Unlock(&s_lock);
	return r;
}

void cache_manager_shutdown(void) {
	if (!s_ready) return;
	LightLock_Lock(&s_lock);
	for (int i = 0; i < s_count; i++)
		if (s_tasks[i].status == DOWNLOAD_STATUS_DOWNLOADING)
			s_tasks[i].status = DOWNLOAD_STATUS_WAITING;
	save_locked();
	LightLock_Unlock(&s_lock);
	s_ready = false;
}

static bool path_taken_locked(const char *path) {
	for (int i = 0; i < s_count; i++) if (!strcmp(s_tasks[i].filepath, path)) return true;
	char part[CACHE_PATH_MAX];
	snprintf(part, sizeof(part), "%s.part", path);
	return file_exists(path, NULL) || file_exists(part, NULL);
}

static int append_locked(const char *bvid, int64_t cid, int64_t aid,
                         const char *title, const char *author, int qn) {
	if (!s_ready || !valid_bvid(bvid) || cid <= 0 ||
	    (qn != 16 && qn != 32)) return -1;
	DownloadTask t;
	memset(&t, 0, sizeof(t));
	snprintf(t.bvid, sizeof(t.bvid), "%s", bvid);
	t.cid = cid; t.aid = aid; t.qn = qn;
	snprintf(t.title, sizeof(t.title), "%s", title && title[0] ? title : "未命名视频");
	snprintf(t.author, sizeof(t.author), "%s", author && author[0] ? author : "未知UP主");
	t.status = DOWNLOAD_STATUS_WAITING;
	t.created_time = (int64_t)time(NULL);

	if (find_media_locked(t.bvid, t.cid) >= 0) return 1;
	if (s_count >= CACHE_MAX_TASKS) return -2;
	char safe_author[80], safe_title[192], dir[CACHE_PATH_MAX];
	safe_component(t.author, safe_author, sizeof(safe_author), "未知UP主");
	safe_component(t.title, safe_title, sizeof(safe_title), "未命名视频");
	snprintf(dir, sizeof(dir), "%s/%s", CACHE_VIDEO_DIR, safe_author);
	if (!ensure_dir(dir)) return -3;
	snprintf(t.filepath, sizeof(t.filepath), "%s/%s.mp4", dir, safe_title);
	if (path_taken_locked(t.filepath)) {
		char cidbuf[24];
		u64_dec((uint64_t)t.cid, cidbuf, sizeof(cidbuf));
		snprintf(t.filepath, sizeof(t.filepath), "%s/%s [%s-%s-%d].mp4", dir,
		         safe_title, t.bvid, cidbuf, t.qn);
		/* 数据库外的孤立文件不自动认领或覆盖。确定性后缀仍冲突时，
		 * 让用户处理该文件比猜测它属于谁安全。 */
		if (path_taken_locked(t.filepath)) {
			return -4;
		}
	}
	s_tasks[s_count++] = t;
	return 0;
}

int cache_manager_enqueue(const char *bvid, int64_t cid, int64_t aid,
                          const char *title, const char *author, int qn) {
	LightLock_Lock(&s_lock);
	int old_count = s_count;
	int r = append_locked(bvid, cid, aid, title, author, qn);
	if (r == 0 && save_locked() != 0) {
		s_count = old_count;
		r = -3;
	}
	LightLock_Unlock(&s_lock);
	return r;
}

int cache_manager_enqueue_batch(const CacheEnqueueItem *items, int count,
                                int *added, int *duplicates) {
	if (added) *added = 0;
	if (duplicates) *duplicates = 0;
	if (!items || count <= 0) return -1;

	LightLock_Lock(&s_lock);
	int old_count = s_count;
	int add_n = 0, dup_n = 0, first_error = 0;
	for (int i = 0; i < count; i++) {
		int r = append_locked(items[i].bvid, items[i].cid, items[i].aid,
		                      items[i].title, items[i].author, items[i].qn);
		if (r == 0) add_n++;
		else if (r == 1) dup_n++;
		else if (!first_error) first_error = r;
	}
	if (add_n && save_locked() != 0) {
		s_count = old_count;
		add_n = 0;
		first_error = -3;
	}
	LightLock_Unlock(&s_lock);
	if (added) *added = add_n;
	if (duplicates) *duplicates = dup_n;
	return first_error;
}

int cache_manager_find(const char *bvid, int64_t cid, DownloadTask *out) {
	if (!bvid || !bvid[0] || cid <= 0) return -1;
	LightLock_Lock(&s_lock);
	int i = find_media_locked(bvid, cid);
	if (i >= 0 && out) *out = s_tasks[i];
	LightLock_Unlock(&s_lock);
	return i >= 0 ? 0 : -1;
}

int cache_manager_count(void) {
	LightLock_Lock(&s_lock); int n = s_count; LightLock_Unlock(&s_lock); return n;
}

int cache_manager_snapshot(DownloadTask *out, int max) {
	if (!out || max <= 0) return 0;
	LightLock_Lock(&s_lock);
	int n = s_count < max ? s_count : max;
	memcpy(out, s_tasks, (size_t)n * sizeof(*out));
	LightLock_Unlock(&s_lock);
	return n;
}

static int set_status(const DownloadTask *key, DownloadStatus from1,
                      DownloadStatus from2, DownloadStatus to) {
	LightLock_Lock(&s_lock);
	int i = find_locked(key);
	if (i < 0 || (s_tasks[i].status != from1 && s_tasks[i].status != from2)) {
		LightLock_Unlock(&s_lock); return -1;
	}
	DownloadStatus old = s_tasks[i].status;
	s_tasks[i].status = to;
	int r = save_locked();
	if (r != 0) s_tasks[i].status = old;
	LightLock_Unlock(&s_lock);
	return r;
}

int cache_manager_pause(const DownloadTask *t) {
	return set_status(t, DOWNLOAD_STATUS_WAITING, DOWNLOAD_STATUS_DOWNLOADING,
	                  DOWNLOAD_STATUS_PAUSED);
}
int cache_manager_resume(const DownloadTask *t) {
	return set_status(t, DOWNLOAD_STATUS_PAUSED, DOWNLOAD_STATUS_PAUSED,
	                  DOWNLOAD_STATUS_WAITING);
}
int cache_manager_retry(const DownloadTask *t) {
	return set_status(t, DOWNLOAD_STATUS_FAILED, DOWNLOAD_STATUS_FAILED,
	                  DOWNLOAD_STATUS_WAITING);
}

int cache_manager_remove(const DownloadTask *key) {
	LightLock_Lock(&s_lock);
	int i = find_locked(key);
	if (i < 0) { LightLock_Unlock(&s_lock); return -1; }
	if (s_tasks[i].status == DOWNLOAD_STATUS_DOWNLOADING) {
		LightLock_Unlock(&s_lock); return -2;
	}
	char final[CACHE_PATH_MAX], part[CACHE_PATH_MAX];
	snprintf(final, sizeof(final), "%s", s_tasks[i].filepath);
	cache_manager_part_path(&s_tasks[i], part, sizeof(part));
	if (remove(final) != 0 && errno != ENOENT) { LightLock_Unlock(&s_lock); return -3; }
	if (remove(part) != 0 && errno != ENOENT) { LightLock_Unlock(&s_lock); return -3; }
	memmove(&s_tasks[i], &s_tasks[i + 1],
	        (size_t)(s_count - i - 1) * sizeof(s_tasks[0]));
	s_count--;
	int r = save_locked();
	LightLock_Unlock(&s_lock);
	return r;
}

int cache_manager_claim_next(DownloadTask *out) {
	LightLock_Lock(&s_lock);
	int pick = -1;
	for (int i = 0; i < s_count; i++)
		if (s_tasks[i].status == DOWNLOAD_STATUS_WAITING &&
		    (pick < 0 || s_tasks[i].created_time < s_tasks[pick].created_time)) pick = i;
	if (pick < 0) { LightLock_Unlock(&s_lock); return 0; }
	s_tasks[pick].status = DOWNLOAD_STATUS_DOWNLOADING;
	*out = s_tasks[pick];
	int r = save_locked();
	if (r != 0) s_tasks[pick].status = DOWNLOAD_STATUS_WAITING;
	LightLock_Unlock(&s_lock);
	return r == 0 ? 1 : -1;
}

bool cache_manager_task_is_downloading(const DownloadTask *key) {
	LightLock_Lock(&s_lock);
	int i = find_locked(key);
	bool yes = i >= 0 && s_tasks[i].status == DOWNLOAD_STATUS_DOWNLOADING;
	LightLock_Unlock(&s_lock);
	return yes;
}

int cache_manager_update_progress(const DownloadTask *key, uint64_t downloaded,
                                  uint64_t total, bool force_save) {
	LightLock_Lock(&s_lock);
	int i = find_locked(key);
	if (i < 0) { LightLock_Unlock(&s_lock); return -1; }
	s_tasks[i].downloaded_size = downloaded;
	if (total) s_tasks[i].total_size = total;
	int r = force_save ? save_locked() : 0;
	LightLock_Unlock(&s_lock);
	return r;
}

static int mark(const DownloadTask *key, DownloadStatus status, uint64_t size) {
	LightLock_Lock(&s_lock);
	int i = find_locked(key);
	if (i < 0) { LightLock_Unlock(&s_lock); return -1; }
	s_tasks[i].status = status;
	if (size) {
		s_tasks[i].downloaded_size = size;
		if (status == DOWNLOAD_STATUS_COMPLETED) s_tasks[i].total_size = size;
	}
	int r = save_locked();
	LightLock_Unlock(&s_lock);
	return r;
}

int cache_manager_mark_waiting(const DownloadTask *t) {
	return mark(t, DOWNLOAD_STATUS_WAITING, 0);
}
int cache_manager_mark_failed(const DownloadTask *t) {
	return mark(t, DOWNLOAD_STATUS_FAILED, 0);
}
int cache_manager_mark_completed(const DownloadTask *t, uint64_t size) {
	return mark(t, DOWNLOAD_STATUS_COMPLETED, size);
}

const char *cache_manager_status_name(DownloadStatus s) {
	switch (s) {
	case DOWNLOAD_STATUS_WAITING: return "等待中";
	case DOWNLOAD_STATUS_DOWNLOADING: return "下载中";
	case DOWNLOAD_STATUS_PAUSED: return "已暂停";
	case DOWNLOAD_STATUS_FAILED: return "失败";
	case DOWNLOAD_STATUS_COMPLETED: return "已完成";
	default: return "未知";
	}
}
