#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 流式播放一个 http(s) mp4 直链(h264+aac)。
 * 阻塞直到播放完或用户按 B 退出。返回 0 正常结束,<0 出错。
 * 需要:gfx 已初始化;NDSP 可用(SD 卡有 dspfirm.cdc 或已内置)。
 * New3DS 优先使用 MVD 硬件解码,失败自动退回软解。 */
int player_play(const char *url, const char *title);
/* 播放 SD 卡上的完整 MP4。只替换输入传输层，后续 NetRing、FFmpeg、
 * MVD/软解、音频与渲染管线和在线播放完全相同。 */
int player_play_file(const char *path, const char *title);

/* 分P。labels/durations 由调用方保管(播放期间必须一直有效),
 * 播放器只读不存。n>1 时下屏左下角的「返回」换成「选集」——
 * 返回本来就有 B 键,而选集在播放中没有别的入口。
 * 选集子页里上屏保持暂停,下屏换成列表。 */
void player_set_pages(const char *const *labels, const int *durations,
                      int n, int cur);
/* 播放结束后问一次:用户在选集里挑了哪一 P(-1 = 没挑,正常退出)。
 * 取走即清。重新取流由 main.c 负责 —— 播放器不碰分P 的数据。 */
int  player_take_page_pick(void);
/* 上一次播放是不是自然播到片尾(而非 B 退出 / 换 P / 出错)。
 * 多 P 视频据此决定要不要自动连播下一集。 */
bool player_ended_naturally(void);

/* 开流阶段(连接 / 载入)会阻塞几秒。播放器自己不知道该画什么界面 ——
 * 那时候还没有画面,而列表页在 main.c 手里。所以由调用方注册一个
 * 「画一帧忙碌状态」的回调:沿用原来那一屏,只在状态条上写在做什么。
 * 不注册的话播放器退回自己画一句提示。 */
void player_set_busy_cb(void (*cb)(const char *msg));

/* 播放偏好(由设置页控制) */
void player_set_prefs(bool danmaku_on, bool force_sw, int qn);
/* 开机调一次:从 SD 存档恢复字幕开关和弹幕/字幕字号 */
void player_prefs_init(void);

/* 当前视频元数据(发弹幕要用 aid+cid) */
void player_set_meta(int64_t aid, int64_t cid, const char *bvid);
/* 登录回调:发弹幕时未登录则调用(阻塞直到登录流程结束),
 * 返回最终是否已登录 */
void player_set_login_cb(bool (*cb)(void));
/* 播放控制页的缓存按钮。all_parts=true 表示选集页“一键缓存全集”；
 * 回调把用户可见结果写入 message，播放器只负责显示，不依赖缓存模块。 */
typedef int (*PlayerCacheCallback)(bool all_parts, char *message, size_t message_len);
void player_set_cache_cb(PlayerCacheCallback cb);

/* 收藏夹列表在开播前由 main.c 安全预取，避免播放长连接和收藏 API 并发。
 * 用户点中目标后播放器安全退出流，main.c 再提交收藏。 */
void player_set_favorite_folders(const int64_t *ids,
                                 const char *const *titles,
                                 const int *counts, int n);
int64_t player_take_favorite_request(void);

/* 定时关机由播放器设置，但在主页也继续计时；两个主循环都调用 poll。 */
void player_shutdown_timer_poll(void);
int  player_shutdown_timer_remaining(void); /* 向上取整的剩余分钟；0=未设置 */

/* APT 钩子调:HOME 挂起时暂停播放(线程不会自动停,见 player.c 的说明) */
void player_notify_suspend(void);

#endif
