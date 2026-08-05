/* bilibili:基于 3Danmu 的非官方视频客户端,运行在自制系统的 3DS 上
 *
 * 操作:
 *   十字键上下   选择视频
 *   A           播放(播放中 A 暂停 / B 退出)
 *   Y           搜索(下屏也有搜索按钮)
 *   X           播放中切换倍速;长按恢复 1.0x
 *   L / R       向上翻一屏 / 手动加载下一批
 *   SELECT      回到热门
 *   START       退出程序
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "net.h"
#include "bili.h"
#include "player.h"
#include "thread_util.h"
#include "danmaku.h"
#include "ime.h"
#include "settings.h"
#include "thumb.h"
#include "subtitle.h"
#include "tls.h"
#include "cache_manager.h"
#include "download_worker.h"
#include "vendor/qrcodegen.h"

#define LIST_PAGE_SIZE 20
/* 列表接口每次只给 20 条，但主页现在采用“滚到底继续追加”。
 * BiliVideo 约 500B，1000 条不到 0.5MB，放普通堆/BSS 对 New 3DS 很轻；
 * 同时给异常接口一个硬上限，不能真的让无穷流把掌机内存吃光。 */
#define MAX_LIST 1000
#define THUMB_WINDOW LIST_PAGE_SIZE

/* 加大主线程栈(默认 32KB,ffmpeg 解码 + 深调用链不够用) */
unsigned int __stacksize__ = 256 * 1024;

/* 【线性堆必须显式声明,且预算里要算上字体】
 * 线性堆(linearAlloc)装的是所有要给硬件看的缓冲。完整清单(实测):
 *   字体图集       8.2MB  ← C2D_FontLoad 把字形表当纹理放这里!
 *                          (曾经漏算它,14MB 的线性堆被字体吃掉大半,
 *                           MVD 连 2.4MB 输入缓冲都分不出来,硬解全灭)
 *   MVD 工作区     5~7MB
 *   MVD 输入/输出  2.4MB
 *   视频纹理+vout  2.0MB
 *   音频/封面      0.5MB
 *   合计峰值      ~20MB
 * CIA 的默认划分远小于此;.3dsx 由 Homebrew Launcher 决定(其 env 参数
 * 优先于本变量,所以这里改大不影响 3dsx)。
 * 25MB 是为了 480P 硬解:L3.1 工作区约 8.5MB,加上字体 8.2 和其余缓冲,
 * 22MB 差一口气。普通堆拿剩下的(约 64-13-25=26MB),硬解路径下 ffmpeg
 * 只做解封装,足够;软解路径帧缓冲也在堆里,实测同样够用。 */
u32 __ctru_linear_heap_size = 25 * 1024 * 1024;

typedef enum { MODE_POPULAR, MODE_RECOMMEND, MODE_SEARCH,
               MODE_HISTORY, MODE_FAV } ListMode;

static BiliVideo s_list[MAX_LIST];
/* 【为什么要多一份 10KB 的暂存】接口层一进来就把 *count 清零、再逐条填,
 * 而那时旧的一页还画在屏幕上。直接写 s_list 的话,翻页途中 s_count 会短暂
 * 归零 —— 上屏于是露出居中的「加载中」,下屏跳出「加载失败?按 A 重试」,
 * 都不是真的。拉到暂存里、成功了才整体换上,翻页就只是某一帧换成新的一页。
 * 20 条 x ~0.5KB,买得起;顺带让 B 取消变成真的无损。 */
static BiliVideo s_stage[LIST_PAGE_SIZE];
static int s_stage_n;
static int  s_count = 0;
static int  s_sel = 0;
static float s_scroll = 0.0f;       /* 列表滚动偏移(显示值,像素) */
static float s_scroll_t = 0.0f;     /* 滚动目标值:输入写这里,显示值每帧缓动跟随 */
#define ROW_H   66.0f               /* 图文卡片行高 */
#define LIST_Y  0.0f                /* 上屏不再画频道标题条，列表从顶端开始 */
#define LIST_H  (240.0f - LIST_Y)
static int  s_page = 1;
/* 已知的最后一页(0 = 还不知道)。热门只有 22 页左右、收藏夹按实际条数,
 * 而接口不会提前告诉你 —— 只能翻到空页那一下才知道。记住它,是为了
 * 之后再按 R 不必再发一次注定为空的请求。换频道/换关键词要清零。 */
static int  s_page_end = 0;
/* 封面模块只保留 20 张 GPU 纹理。长列表用窗口映射：滚到下一批时复用
 * 这 20 个槽，而不是按 1000 条一次性分配线性内存。 */
static int s_thumb_base = -1;
static int s_thumb_n = 0;
static ListMode s_mode = MODE_RECOMMEND;   /* 默认进推荐(和官方 App 一致) */
static int s_pending_mode = -1;   /* 登录界面里点了频道:退出后切过去 */
static int s_hl_mode = -1;        /* 高亮覆盖:点击后立刻亮新的(-1=跟随 s_mode) */
static char s_keyword[128] = {0};
static char s_status[192] = "";
/* 名字以数字开头,所以宏名不能叫 3DANMU_VERSION(C 标识符不许数字打头) */
#define APP_VERSION "1.7.0"

/* ---------- 分 P ----------
 * 与持久化队列上限一致，确保“一键缓存分P”拿到的不是当前可视区或前
 * 200 项，而是 pagelist 一次返回的完整分P。全部放静态区，避免把
 * 几百条标题压进播放器的调用栈。 */
#define MAX_PAGES CACHE_MAX_TASKS
static BiliPage s_pages[MAX_PAGES];
static int s_npages = 0;
/* 给播放器的选集列表。播放器只读不存,所以这两个数组必须活得比播放久 ——
 * 放文件作用域,别改成栈上的临时变量。 */
static char  s_pg_label[MAX_PAGES][72];
static const char *s_pg_labelp[MAX_PAGES];
static int   s_pg_dur[MAX_PAGES];
static DownloadTask s_cache_view[CACHE_MAX_TASKS];
static CacheEnqueueItem s_batch_items[MAX_PAGES];
static char s_batch_titles[MAX_PAGES][200];
static BiliCollection s_collection_info;
static BiliVideo s_collection_videos[CACHE_MAX_TASKS];
static int s_collection_n = 0;
static int s_collection_resolve_at = -1;
#define MAX_PLAYER_FAV_FOLDERS 64
static BiliFavFolder s_player_fav_folders[MAX_PLAYER_FAV_FOLDERS];
static int64_t s_player_fav_ids[MAX_PLAYER_FAV_FOLDERS];
static const char *s_player_fav_titles[MAX_PLAYER_FAV_FOLDERS];
static int s_player_fav_counts[MAX_PLAYER_FAV_FOLDERS];
static int s_player_fav_n = 0;
static bool s_player_fav_loaded = false;
/* 播放器内的缓存按钮只回调入队，不直接依赖缓存模块。这里保存本次播放
 * 对应的视频身份；播放结束立即清空，避免陈旧目标串到下一条视频。 */
static BiliVideo *s_play_cache_video = NULL;
static int64_t s_play_cache_cid = 0;
static int s_play_cache_qn = 16;

static bool g_danmaku = true;       /* 设置:弹幕开关 */
/* 清晰度。B 站的 qn 编号:16=360P 32=480P。
 * 【别再加 240P(qn=6)】试过,撤了:**绝大多数视频根本不提供这一档**,
 * 接口不会报错,而是默默返回 360P —— 设置页显示「240P」而实际是 360P,
 * 用户只会觉得「选了没用」。一个大部分时候什么都不做的选项,
 * 比没有这个选项更糟。相关的判断留在 log_quality 里(bili.c),
 * 哪天 B 站放开了能一眼看出来。 */
static int  g_qn = 16;
#define QN_360 16
#define QN_480 32
static const char *qn_name(int qn) { return qn == QN_480 ? "480P" : "360P"; }
static int qn_next(int qn) { return qn == QN_480 ? QN_360 : QN_480; }
static bool g_force_sw = false;     /* 设置:强制软解 */
static bool s_in_settings = false;  /* 当前在设置页 */
static bool s_debug_ui = false;
static bool s_net_ok = false;       /* httpc 真正初始化成功后才允许调用 bili */
static bool s_cache_ok = false;     /* 缓存目录和任务数据库可用 */

/* 下屏 console 只支持 ASCII:log 打英文,中文状态只画在上屏 */
static char s_busy[128] = "";  /* 状态条:正在做什么 / 上次失败原因(空=空闲) */

static void set_status(const char *ui_utf8, const char *log_ascii) {
	snprintf(s_status, sizeof(s_status), "%s", ui_utf8);
	if (log_ascii) printf("%s\n", log_ascii);
}

static void busy_frame(const char *msg);   /* 定义在下面 */
static int  run_bg(int (*fn)(void *), void *arg, const char *msg);  /* 同上 */
static bool s_job_cancelled;   /* 上一次 run_bg 是否被 B 掐掉,定义在下面 */
static void play_video(BiliVideo *v);
static void play_selected(void);
static void collection_page(BiliVideo *origin);
static void page_label(const BiliPage *pg, char *out, size_t n);

/* 列表加载最多试几次(含第一次)。3 次、退避 0.8s+1.6s —— 再多就该让
 * 用户自己决定了:接口真的挂了的话,机器替他干等只是把失败拖得更久。 */
#define LIST_RETRY_MAX 3

/* 后台作业的参数。这几个调用都在主线程上串行发起,同一时刻只有一个
 * 在跑,所以用文件作用域的变量传参最省事 —— 不必为每种作业各建一个结构。 */
static const char *s_job_bvid;
static int64_t s_job_cid;
static char *s_job_url;
static int s_job_qn;
static BiliVideo *s_job_video;
static int64_t s_job_fav_aid;
static int64_t s_job_fav_id;

static int pagelist_job(void *u) {
	(void)u;
	return bili_pagelist(s_job_bvid, s_pages, MAX_PAGES, &s_npages);
}
static int playurl_job(void *u) {
	(void)u;
	return bili_get_play_url(s_job_bvid, s_job_cid, s_job_qn, s_job_url, 2048);
}
static int video_cid_job(void *u) {
	(void)u;
	return s_job_video ? bili_get_cid(s_job_video->bvid, &s_job_video->cid,
	                                  &s_job_video->aid) : -1;
}
static int collection_job(void *u) {
	(void)u;
	return bili_collection(s_job_bvid, &s_collection_info,
	                       s_collection_videos, CACHE_MAX_TASKS,
	                       &s_collection_n);
}
static int collection_resolve_job(void *u) {
	(void)u;
	for (int i = 0; i < s_collection_n; i++) {
		s_collection_resolve_at = i;
		if (!s_collection_videos[i].cid &&
		    bili_get_cid(s_collection_videos[i].bvid,
		                 &s_collection_videos[i].cid,
		                 &s_collection_videos[i].aid) != 0)
			return -1;
	}
	s_collection_resolve_at = -1;
	return 0;
}
static int fav_folders_job(void *u) {
	(void)u;
	s_player_fav_n = 0;
	return bili_fav_folders(s_player_fav_folders, MAX_PLAYER_FAV_FOLDERS,
	                        &s_player_fav_n);
}
static int fav_add_job(void *u) {
	(void)u;
	return bili_fav_add(s_job_fav_aid, s_job_fav_id);
}

static void list_thumbs_stop(void) {
	thumb_stop();
	s_thumb_base = -1;
	s_thumb_n = 0;
}

static void list_thumbs_start(int around) {
	if (s_count <= 0 || net_is_shutting_down()) {
		list_thumbs_stop();
		return;
	}
	if (around < 0) around = 0;
	if (around >= s_count) around = s_count - 1;
	int base = (around / THUMB_WINDOW) * THUMB_WINDOW;
	int n = s_count - base;
	if (n > THUMB_WINDOW) n = THUMB_WINDOW;
	thumb_start(&s_list[base], n);
	s_thumb_base = base;
	s_thumb_n = n;
}

static void list_thumbs_ensure(int first_visible) {
	if (s_count <= 0) return;
	if (first_visible < s_thumb_base ||
	    first_visible >= s_thumb_base + s_thumb_n)
		list_thumbs_start(first_visible);
}

static const C2D_Image *list_thumb_get(int index) {
	int local = index - s_thumb_base;
	return (local >= 0 && local < s_thumb_n) ? thumb_get(local) : NULL;
}

/* 列表拉取的实体。放到后台线程上跑,主线程照常画帧 —— 直接在主线程调
 * 会冻住整整一秒(实测接口 0.8~1.1s),那一秒和死机分不出来。 */
static int list_fetch_job(void *unused) {
	(void)unused;
	switch (s_mode) {
	case MODE_SEARCH:  return bili_search(s_keyword, s_page, s_stage, LIST_PAGE_SIZE, &s_stage_n);
	case MODE_RECOMMEND: return bili_recommend(s_page, s_stage, LIST_PAGE_SIZE, &s_stage_n);
	case MODE_HISTORY: return bili_history(s_page, s_stage, LIST_PAGE_SIZE, &s_stage_n);
	case MODE_FAV:     return bili_fav(s_page, s_stage, LIST_PAGE_SIZE, &s_stage_n);
	default:           return bili_popular(s_page, s_stage, LIST_PAGE_SIZE, &s_stage_n);
	}
}

static void load_list(void) {
	bool append = s_page > 1 && s_count > 0;
	if (!s_net_ok) {
		if (!append) s_count = 0;
		set_status("网络未就绪(A 重试 / 可打开离线下载任务)",
		           "network unavailable; A=retry");
		snprintf(s_busy, sizeof(s_busy), "网络初始化失败，离线缓存仍可播放");
		return;
	}
	/* 【末页信息收在这一处判定】换频道、换关键词、重新搜索……每条路径
	 * 都会把 s_page 归 1,所以"回到第一页"就是"换了一批内容"的充要条件。
	 * 分散到那 9 个赋值点各写一句,必然漏 —— 这一版就当场漏了两个
	 * (ZL/ZR 切频道那两行是单行 if,和其他写法不一样)。
	 * 同一条规矩写在多处、漏在某处,今天已经在线程回收上栽过一次了。 */
	if (s_page == 1) s_page_end = 0;
	set_status("加载中...", "loading...");
	/* 【翻页时别清屏】先画一帧:**列表原样留着**,只在下屏状态条上提示。
	 * 原来这里把整个上屏清成一句「加载中…」,翻一页闪一次白 ——
	 * 而列表内容其实还在,清掉它只是让人以为东西没了。
	 * 首次加载是例外(还没有列表可留),那时 draw_list 会居中显示 s_status。 */
	const char *waiting = (s_count > 0) ? "加载新一页中" : "加载中";
	busy_frame(waiting);

	/* 【顺序要紧】先停封面,再发接口请求。
	 * 反过来的话,换页时上一页的封面线程会和新的 API 请求并发 ——
	 * 而「图片请求不与 API 请求同时在飞」正是下面放开图片并发的前提。
	 * 这个不变式靠调用顺序保证,比靠一把全局大锁便宜得多。
	 * 放在画帧**之后**:这样那一帧里封面还在,不会先闪掉一排图。 */
	list_thumbs_stop();

	/* 【自动重试】列表接口偶发失败是常态:3DS 的 httpc 本就脆,
	 * B 站对掌机 UA 也时不时风控性地拒一次。这种失败重来一次多半就好了,
	 * 让用户自己按 A 只是把一件机器该做的事推给他。
	 * 但重试必须**可中断**:等待期间照常画帧、B 键随时放弃,
	 * 否则网络真断了就成了一个按不动的三秒卡顿。 */
	int r = -1;
	for (int attempt = 0; ; attempt++) {
		/* 翻页时不提示:列表原样留着,几百毫秒后换成新的一页就是了。
		 * 首次加载没有列表可留,那时才需要一句「加载中」。 */
		r = run_bg(list_fetch_job, NULL, waiting);
		if (r == 0 || attempt >= LIST_RETRY_MAX - 1) break;
		if (s_job_cancelled) break;   /* 用户主动取消:不是错误,别重试 */
		if (net_is_shutting_down() || aptShouldClose()) break;

		/* 退避 0.8s / 1.6s:两次之间不留间隔的话,失败往往只是重复一遍 */
		bool give_up = false;
		char m[96];
		snprintf(m, sizeof(m), "加载失败,重试中 %d/%d(B 放弃)",
		         attempt + 2, LIST_RETRY_MAX);
		u64 t0 = osGetTime();
		u32 wait_ms = 800u * (u32)(attempt + 1);
		while (osGetTime() - t0 < wait_ms) {
			/* 这是我们自己的子循环,一帧扫一次输入 —— 和登录页同一套规矩 */
			hidScanInput();
			if (hidKeysDown() & KEY_B) { give_up = true; break; }
			if (net_is_shutting_down() || aptShouldClose()) { give_up = true; break; }
			busy_frame(m);
		}
		if (give_up) break;
	}

	/* 【接口成功但一条都没有 = 到底了,不是出错】热门只有 22 页左右
	 * (B 站自己的上限),收藏/历史/搜索同理。原来这里照样提交,于是
	 * s_count 变 0:上屏空白、下屏跳「加载失败?按 A 重试」,而接口
	 * 明明是好的 —— 用户按 A 重试多少次都是同一个空页。
	 * 改成留住上一页并把页码退回去:再按一次翻页不会越走越远。 */
	if (r == 0 && s_stage_n == 0 && s_count > 0) {
		/* 退回来源那一页。空页只可能是 R 翻过头翻出来的(L 只会往回走,
		 * 走到的都是有内容的页),所以减一就是用户刚才待着的地方。 */
		if (s_page > 1) s_page--;
		s_page_end = s_page;      /* 记下来,下次 R 直接挡掉 */
		set_status("", "no more pages");
		snprintf(s_busy, sizeof(s_busy), "已经是最后一页");
		if (s_count > 0) list_thumbs_start(s_sel);
		return;
	}
	if (r == 0) {
		if (append) {
			int added = 0;
			for (int j = 0; j < s_stage_n && s_count < MAX_LIST; j++) {
				bool duplicate = false;
				for (int i = 0; i < s_count; i++) {
					if (!strcmp(s_list[i].bvid, s_stage[j].bvid)) {
						duplicate = true;
						break;
					}
				}
				if (!duplicate) {
					s_list[s_count++] = s_stage[j];
					added++;
				}
			}
			if (!added || s_count >= MAX_LIST) {
				s_page_end = s_page;
				if (s_count >= MAX_LIST)
					snprintf(s_busy, sizeof(s_busy), "列表已达到本机安全上限");
			}
		} else {
			memcpy(s_list, s_stage, (size_t)s_stage_n * sizeof(s_stage[0]));
			s_count = s_stage_n;
		}
	}
	if (!append && (r == 0 || !s_job_cancelled)) {
		/* 取消时连滚动位置一起留着 —— 那才叫"当无事发生" */
		s_sel = 0;
		s_scroll = 0.0f;
		s_scroll_t = 0.0f;
	}
	if (r != 0 && s_job_cancelled) {
		/* 取消了就当无事发生:保留原来的列表,状态条清干净 */
		set_status("", "load cancelled");
		s_busy[0] = 0;
	} else if (r != 0) {
		if (append && s_page > 1) s_page--;  /* 下次仍重试同一页，不跳号 */
		if (!append) s_count = 0;
		char sbuf[160];
		snprintf(sbuf, sizeof(sbuf), "加载失败 %s(A 重试 / B 返回)",
		         bili_last_error());
		set_status(sbuf, "load failed, A=retry B=back");
		snprintf(s_busy, sizeof(s_busy), "加载失败:%s", bili_last_error());
	} else {
		char buf[64];
		snprintf(buf, sizeof(buf), "loaded page %d, %d items", s_page, s_count);
		set_status("", buf);
		if (append && s_page_end && s_page >= s_page_end) {
			if (!s_busy[0]) snprintf(s_busy, sizeof(s_busy), "没有更多新内容");
		} else {
			s_busy[0] = 0;  /* 加载成功:清掉上次的失败原因 */
		}
	}
	if (s_count > 0)
		list_thumbs_start(s_sel);
}

static void fmt_meta(const BiliVideo *v, char *out, size_t n) {
	char views[32] = "";
	if (v->views >= 100000000)
		snprintf(views, sizeof(views), " %.1f亿", v->views / 1e8);
	else if (v->views >= 10000)
		snprintf(views, sizeof(views), " %.1f万", v->views / 1e4);
	else if (v->views >= 0)
		snprintf(views, sizeof(views), " %ld", (long)v->views);
	char dur[24] = "";
	if (v->duration > 0)
		snprintf(dur, sizeof(dur), "  %d:%02d", v->duration / 60, v->duration % 60);
	snprintf(out, n, "%s%s播放%s", v->author, views, dur);
}

static void draw_list(void) {
	ui_begin();
	/* 封面上传预算:静止时每帧 1 张;滚动进行中为 0——同步 GPU 传输
	 * 哪怕 1-2ms 也会在滚动时被肉眼捕捉为顿挫,滚完再补 */
	{
		float d = s_scroll - s_scroll_t;
		thumb_new_frame((d > -1.0f && d < 1.0f) ? 1 : 0);
	}
	/* 层级:citro2d 深度测试为 GEQUAL —— z 大者在上,同 z 后画者胜。
	 * 实测数据:选中高亮 z=0.4 时会盖掉缩略图(z=0.2),行文字(z=0.5)
	 * 会穿透后画的顶栏(z=0.4)。所以:高亮 0.15 < 图 0.2 < 文字 0.5
	 * < 顶栏底 0.6 < 顶栏字 0.7 */

	/* 列表:图文卡片,s_scroll 像素级滚动(左摇杆) */
	{
		int first = (int)(s_scroll / ROW_H);
		if (first < 0) first = 0;
		for (int i = first; i < s_count; i++) {
			/* 坐标取整:小数位置会让文字/边缘在像素间跳,滚动看着发抖 */
			float y = (float)(int)(LIST_Y + i * ROW_H - s_scroll);
			if (y > 240) break;
			if (i == s_sel)
				ui_rect_z(0, y, 0.15f, 400, ROW_H - 4, UI_COL_SEL);
			const C2D_Image *im = list_thumb_get(i);
			if (im) {
				C2D_DrawImageAt(*im, 6, y + 2, 0.2f, NULL, 1.0f, 1.0f);
			} else {
				ui_rect_z(6, y + 2, 0.18f, THUMB_W, THUMB_H,
				          C2D_Color32(0x30, 0x30, 0x3C, 0xFF));
				ui_text(6 + 38, y + 24, 0.45f, UI_COL_DIM, "…");
			}
			if (i == s_sel) {
				/* 选中行:标题过长时缓慢跑马灯(停 1.2s → 滚动 →
				 * 到尾停 1s → 回起点)。裁剪住行框,不糊到封面/顶栏 */
				static float marq = 0;
				static int marq_sel = -1;
				static u64 marq_t0 = 0;
				static float marq_tw = 0;   /* 标题宽度:换行才重算 */
				static size_t marq_len = 0; /* 换页后同下标不同视频也要重算 */
				size_t tlen = strlen(s_list[i].title);
				if (marq_sel != i || marq_len != tlen) {
					marq_sel = i;
					marq_len = tlen;
					marq = 0;
					marq_t0 = osGetTime();
					/* 量宽是完整的字形解析,别每帧都做 */
					marq_tw = ui_text_width(s_list[i].title, UI_SHARP);
				}
				float tw = marq_tw;
				const float mw = 286.0f;
				if (tw > mw) {
					float span = tw - mw + 12.0f;
					if (osGetTime() - marq_t0 > 1200) {
						marq += 0.55f;
						if (marq > span + 55.0f) {   /* 尾部停顿后回起点 */
							marq = 0;
							marq_t0 = osGetTime();
						}
					}
					float off = marq > span ? span : marq;
					ui_clip(110, y + 2, mw, 24);
					ui_text(110 - off, y + 2, UI_SHARP, UI_COL_WHITE,
					        s_list[i].title);
					ui_unclip();
				} else {
					ui_text(110, y + 2, UI_SHARP, UI_COL_WHITE,
					        s_list[i].title);
				}
			} else {
				ui_text_clipped(110, y + 2, UI_SHARP, UI_COL_TEXT,
				                s_list[i].title, 286);
			}
			char meta[128];
			fmt_meta(&s_list[i], meta, sizeof(meta));
			ui_text_clipped(110, y + 42, UI_SHARP, UI_COL_DIM, meta, 286);
		}
	}
	if (s_count == 0)
		ui_text(120, 110, UI_SHARP, UI_COL_DIM, s_status);

}

typedef struct {
	bool login, settings, hist, fav, popular, recommend, search;
	bool cache, parts, downloads;
} ListActions;

static void draw_bottom_list(bool touched, float tx, float ty, ListActions *act) {
	ui_begin_bottom();
	ui_text(10, 4, UI_SHARP, UI_COL_TEXT, bili_logged_in() ? "已登录" : "未登录");
	if (s_count == 0)
		ui_text(108, 4, UI_SHARP, UI_COL_ACCENT, "加载失败?按 A 重试");
	{	/* 底部状态条:在做什么 + 总进度 */
		int done = 0, total = 0;
		bool busy = thumb_progress(&done, &total);
		ui_rect(10, 198, 300, 36, C2D_Color32(0x26, 0x26, 0x30, 0xFF));
		if (s_busy[0]) {  /* 当前动作 或 上次失败原因 */
			ui_text_clipped_z(18, 206, 0.55f, UI_SHARP, UI_COL_WHITE,
			                  s_busy, 284);
		} else if (busy && total > 0) {
			/* 只写文字,不涂进度条:封面是逐张浮现的,列表本身就是进度,
			 * 底下再来一条爬动的色块只是抢注意力 */
			char pbuf[48];
			snprintf(pbuf, sizeof(pbuf), "加载封面 %d/%d", done, total);
			ui_text_z(18, 206, 0.55f, UI_SHARP, UI_COL_WHITE, pbuf);
		} else {
			/* 空闲时兼作按键说明(别再单画一行,会和条重叠——踩过) */
			ui_text_z(18, 206, 0.55f, UI_SHARP, UI_COL_DIM,
			          s_count ? "A播放 Y/触屏搜索 L/R翻页"
			                  : "等待加载(A 重试)");
		}
	}
	/* 高亮 = s_hl_mode(点击后立刻切过去,连扫码登录期间也跟着走),
	 * 未设置时跟随实际频道 s_mode */
	int hl = (s_hl_mode >= 0) ? s_hl_mode : (int)s_mode;
	if (ui_button(10, 28, 96, 36, "推荐",
	              hl == MODE_RECOMMEND ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty)) {
		act->recommend = true;
		s_hl_mode = MODE_RECOMMEND;
	}
	if (ui_button(112, 28, 96, 36, "热门",
	              hl == MODE_POPULAR ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty)) {
		act->popular = true;
		s_hl_mode = MODE_POPULAR;
	}
	if (ui_button(214, 28, 96, 36, "搜索",
	              hl == MODE_SEARCH ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty))
		act->search = true;
	if (ui_button(10, 70, 96, 36, "历史",
	              hl == MODE_HISTORY ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty)) {
		act->hist = true;
		s_hl_mode = MODE_HISTORY;
	}
	if (ui_button(112, 70, 96, 36, "收藏",
	              hl == MODE_FAV ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty)) {
		act->fav = true;
		s_hl_mode = MODE_FAV;
	}
	if (ui_button(214, 70, 96, 36, "设置", UI_COL_SEL, touched, tx, ty))
		act->settings = true;
	if (ui_button(10, 112, 96, 36, bili_logged_in() ? "注销" : "扫码登录",
	              UI_COL_SEL, touched, tx, ty))
		act->login = true;
	if (ui_button(112, 112, 96, 36, "缓存当前", UI_COL_SEL,
	              touched, tx, ty))
		act->cache = true;
	if (ui_button(214, 112, 96, 36, "查看合集", UI_COL_SEL,
	              touched, tx, ty))
		act->parts = true;
	if (ui_button(10, 154, 300, 36, "离线下载任务", UI_COL_SEL,
	              touched, tx, ty))
		act->downloads = true;
}

/* ---------- 设置页 ----------
 * 下屏是可滚动列表,上屏是「你刚碰的那一项」的说明。
 * 双屏机器上「下屏操作、上屏解释」是最自然的分工 —— 像「解码方式」
 * 「画面比例」这类一句话说不清的选项,以前只能靠用户点了看效果去猜。
 *
 * 说明文字要**预先折好行**:上屏可用宽度 384px,汉字在清晰档约 18px 一个,
 * 一行放得下 20 个左右。自动折行要按字宽逐字量,而这些文字是写死的,
 * 写的时候折一次比每帧算一次划算。 */
enum { SET_DANMAKU, SET_QN, SET_DECODE, SET_CACHE, SET_DEBUG, SET_BACK, SET_N };

static void settings_rows(UiRow *r, char *cachebuf, size_t cbn) {
	if (thumb_cache_clearing())
		/* 带上百分比:光一句「清理中…」分不出是在干活还是卡住了,
		 * 而缓存里几千个文件时它本来就要转好一会儿 */
		snprintf(cachebuf, cbn, "清理中 %d%%", thumb_cache_clear_pct());
	else
		snprintf(cachebuf, cbn, "清理 %dMB", (int)(thumb_cache_kb() / 1024));
	r[SET_DANMAKU] = (UiRow){ "弹幕", g_danmaku ? "开" : "关",
		"是否加载并显示弹幕。\n关掉可以省下加载时间和\n绘制开销。" };
	r[SET_QN]      = (UiRow){ "清晰度", qn_name(g_qn),
		"画面的分辨率。\n480P 更清晰,需要登录,\n也更吃解码性能。\n"
		"取流失败会自动回落 360P。" };
	r[SET_DECODE]  = (UiRow){ "解码方式", g_force_sw ? "强制软解" : "自动",
		"自动 = 有硬件解码器就用它,\n失败自动退回软件解码。\n"
		"软解 = 一律用 CPU 解码,\n兼容性最好,但慢很多。\n\n"
		"通常保持「自动」即可。" };
	r[SET_CACHE]   = (UiRow){ "封面缓存", cachebuf,
		"列表封面存在 SD 卡上,\n下次遇到同一个视频就不用\n重新下载。\n"
		"点一下清空;超过 100MB\n也会自动清。" };
	r[SET_DEBUG]   = (UiRow){ "调试台", NULL,
		"切到全屏日志页。\n记录最近 400 行:网络请求、\n解码状态、错误码。\n"
		"出问题时把它拍下来,\n比任何文字描述都有用。\n手指拖动滚动,双击退出。" };
	r[SET_BACK]    = (UiRow){ "返回", NULL, "回到视频列表。\nB 键同样可以。" };
}

static void draw_settings(bool touched, bool holding, bool released,
                          float tx, float ty) {
	UiRow rows[SET_N];
	char cachebuf[32];
	settings_rows(rows, cachebuf, sizeof(cachebuf));

	/* 上屏:说明。没选中任何一项时给一段总览,别留空屏 */
	int f = ui_list_focus();
	const char *ttl = (f >= 0 && f < SET_N) ? rows[f].name : "设置";
	const char *body = (f >= 0 && f < SET_N) ? rows[f].help
	                 : "下屏点按修改。\n碰哪一项,这里就说明哪一项。";
	char brand[32];
	snprintf(brand, sizeof(brand), "bilibili v%s", APP_VERSION);
	ui_help_draw(ttl, body, "仅供学习交流  严禁用于商业用途", brand);

	if (ui_console_active()) return;   /* 调试台占着下屏 */
	int hit = ui_list_draw("设置", rows, SET_N, touched, holding, released, tx, ty);
	switch (hit) {
	case SET_DANMAKU:
		g_danmaku = !g_danmaku;
		settings_set("danmaku", g_danmaku);
		break;
	case SET_QN:
		g_qn = qn_next(g_qn);
		settings_set("qn", g_qn);
		break;
	case SET_DECODE:
		g_force_sw = !g_force_sw;
		settings_set("force_sw", g_force_sw);
		break;
	case SET_CACHE:
		/* 【异步】目录里可能有几万个文件,同步删会把界面冻住 */
		thumb_cache_clear_async();
		break;
	case SET_DEBUG:
		/* 帧结束后再切:当帧内切会被 GPU 覆盖成花屏 */
		s_debug_ui = true;
		break;
	case SET_BACK:
		s_in_settings = false;
		break;
	default: break;
	}
}

static void do_search(void) {
	if (!s_net_ok) { load_list(); return; }
	char input[128] = {0};
	/* 自带触屏拼音输入法(词库 romfs:/pinyin.dic);
	 * 词库缺失时退化为英文键盘,不再依赖 3DS 系统键盘 */
	if (ime_input("搜索视频", s_keyword, input, sizeof(input)) && input[0]) {
		snprintf(s_keyword, sizeof(s_keyword), "%s", input);
		s_mode = MODE_SEARCH;
		s_page = 1;
		load_list();
	}
}

static bool confirm_logout(void) {
	while (aptMainLoop()) {
		player_shutdown_timer_poll();
		hidScanInput();
		u32 kd = hidKeysDown();
		touchPosition tp = { 0, 0 };
		bool touched = (kd & KEY_TOUCH) != 0;
		if (touched) hidTouchRead(&tp);

		ui_begin();
		ui_rect(38, 68, 324, 104, C2D_Color32(0x20, 0x20, 0x2A, 0xF4));
		ui_text(118, 88, UI_SHARP, UI_COL_WHITE, "确认退出登录？");
		ui_text(84, 128, 0.65f, UI_COL_DIM,
		        "退出后历史、收藏等账号功能将不可用");
		ui_begin_bottom();
		ui_text(84, 52, UI_SHARP, UI_COL_TEXT, "是否注销当前 bilibili 账号");
		bool yes = ui_button(20, 112, 132, 46, "确认注销", UI_COL_ACCENT,
		                     touched, tp.px, tp.py);
		bool no = ui_button(168, 112, 132, 46, "取消", UI_COL_SEL,
		                    touched, tp.px, tp.py);
		ui_text(73, 188, 0.65f, UI_COL_DIM, "A 确认   B 取消");
		ui_end();

		if (yes || (kd & KEY_A)) return true;
		if (no || (kd & KEY_B)) return false;
	}
	return false;
}

static void do_login(void) {
	if (!s_net_ok) { load_list(); return; }
	if (bili_logged_in()) {
		if (confirm_logout()) {
			bili_logout();
			s_player_fav_n = 0;
			s_player_fav_loaded = false;
			player_set_favorite_folders(NULL, NULL, NULL, 0);
			set_status("已注销", "logged out");
		} else {
			set_status("已取消注销", "logout cancelled");
		}
		return;
	}

	char qurl[512], qkey[64];
	if (bili_qr_generate(qurl, sizeof(qurl), qkey, sizeof(qkey)) != 0) {
		set_status("获取二维码失败", "QR generate failed");
		return;
	}

	uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
	uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
	if (!qrcodegen_encodeText(qurl, tmp, qr, qrcodegen_Ecc_MEDIUM,
	                          1, 10, qrcodegen_Mask_AUTO, true)) {
		set_status("二维码生成失败", "QR encode failed");
		return;
	}

	printf("scan QR to log in, B to cancel\n");
	s_pending_mode = -1;   /* 清掉上次残留,否则会污染本次结果 */
	u64 last_poll = 0;
	while (aptMainLoop()) {
		player_shutdown_timer_poll();
		/* 一帧只能 scan 一次!scan 两次会把触摸的"按下沿"吃掉
		 * (第二次 kDown 已经变 0),表现为下屏按钮完全点不动 */
		hidScanInput();
		u32 kd = hidKeysDown();
		if (kd & KEY_B) { set_status("已取消登录", "login cancelled"); return; }
		touchPosition tp = { 0, 0 };
		bool touched = (kd & KEY_TOUCH) != 0;
		if (touched) hidTouchRead(&tp);

		ui_begin();
		ui_text(96, 8, UI_SHARP, UI_COL_TEXT, "手机 B 站 App 扫码登录");
		ui_qr(qr, 200, 128, 4);
		ui_text(104, 222, UI_SHARP, UI_COL_DIM, "B 取消 / 下屏可直接切换频道");
		/* 下屏就是平时那张主页面(不另做界面):点哪个频道就切哪个,
		 * 高亮跟着走,登录随之取消 */
		ListActions act = { 0 };
		if (!ui_console_active())
			draw_bottom_list(touched, tp.px, tp.py, &act);
		ui_end();
		if (act.popular || act.recommend || act.hist || act.fav) {
			/* 历史/收藏仍需登录,登录未完成时点它们无意义 → 只接受热门/推荐 */
			if (act.popular || act.recommend) {
				s_pending_mode = act.popular ? MODE_POPULAR : MODE_RECOMMEND;
				set_status("已取消登录", "login cancelled");
				return;
			}
		}
		if (act.settings) {   /* 设置在登录页点了也当取消 */
			set_status("已取消登录", "login cancelled");
			return;
		}
		if (act.search) {
			set_status("已取消登录", "login cancelled");
			do_search();
			return;
		}

		u64 now = osGetTime();
		if (now - last_poll >= 2000) {
			last_poll = now;
			int code = -1;
			if (bili_qr_poll(qkey, &code) == 0) {
				if (code == 0) {
					/* code==0 只说明「扫码这一步过了」,不代表 cookie 拿到手了。
					 * 以前这里无条件报「登录成功」,于是出问题时屏幕说成功、
					 * 顶栏说未登录 —— 提示本身在骗人,白白多查了一轮 */
					if (bili_logged_in()) {
						s_player_fav_loaded = false;
						set_status("登录成功", "login OK");
					} else {
						set_status("登录未生效,请重试", "login failed: cookies rejected");
						printf("qr: code=0 but nav says not logged in\n");
					}
					return;
				}
				if (code == 86038) {
					/* 过期,换一张 */
					if (bili_qr_generate(qurl, sizeof(qurl), qkey, sizeof(qkey)) != 0 ||
					    !qrcodegen_encodeText(qurl, tmp, qr, qrcodegen_Ecc_MEDIUM,
					                          1, 10, qrcodegen_Mask_AUTO, true)) {
						set_status("二维码刷新失败", "QR refresh failed");
						return;
					}
					printf("QR refreshed\n");
				} else if (code == 86090) {
					printf("scanned, confirm on phone\n");
				}
			}
		}
	}
}

/* 阻塞网络操作前调用:立即画一帧,把"正在做什么"刷到状态条上 */
/* 【换 P 时别退回列表页】置位后 busy_frame 只画一句提示,不画视频列表。
 * 在播放中换 P,视觉上应该是「同一部片子换一集」,而闪回一整屏列表
 * 会让人以为已经退出播放了 —— 何况一两秒后又跳回播放器,更乱。 */
static bool s_busy_minimal = false;

/* ---------- 把阻塞的网络调用挪出主线程 ----------
 * 取列表、查分P、解析播放地址都是同步 HTTP,实测每次 0.8~1.1 秒。
 * 直接在主线程上调,那一秒界面完全冻住 —— 连"正在做什么"的动画都不动,
 * 和死机没法区分(这正是被反复反馈的"卡住")。
 *
 * 做法不是把整套流程改成异步(那要重写调用链),而是**把这一次调用
 * 放到临时线程上,主线程原地转着画帧**。逻辑顺序完全不变,
 * 调用方拿到的还是同一个返回值,只是期间界面活着。 */
typedef struct {
	int (*fn)(void *);
	void *arg;
	volatile int done;
	int ret;
} AsyncJob;

static void async_job_thread(void *p) {
	AsyncJob *j = (AsyncJob *)p;
	j->ret = j->fn(j->arg);
	__dmb();
	j->done = 1;
}

/* 返回 fn 的返回值。建不出线程就退回同步调用 —— 慢一点也比不能用强。 */
/* 上一次 run_bg 是不是被用户按 B 掐掉的。调用方据此决定「失败」要不要
 * 报错、要不要重试 —— 主动取消不是错误,不该弹「加载失败」也不该自动重来。 */
static int run_bg(int (*fn)(void *), void *arg, const char *msg) {
	AsyncJob job = { fn, arg, 0, -1 };
	/* 优先级比主线程低一档:它只是在等网络,不该和界面抢 */
	Thread th = NULL;
	static const int cores[] = { 2, 3, -2 };
	for (int i = 0; i < 3 && !th; i++)
		th = threadCreate(async_job_thread, &job, 16 * 1024, 0x31, cores[i], false);
	if (!th) return fn(arg);
	static const char *dots[4] = { "", ".", "..", "..." };
	bool cancelling = false;
	s_job_cancelled = false;
	while (!job.done && aptMainLoop()) {
		player_shutdown_timer_poll();
		hidScanInput();
		/* 【B 取消】httpc 的请求既没有超时也不看标志位,唯一能中断它的
		 * 办法是从外面把连接掐掉 —— net_cancel_streams 就是干这个的
		 * (挂起和退出路径本来就在用它,是条验证过的路)。
		 * 掐完那个阻塞调用会很快带着错误返回,于是这个循环自然结束。 */
		if (!cancelling && (hidKeysDown() & KEY_B)) {
			cancelling = true;
			s_job_cancelled = true;
			net_cancel_streams();
		}
		char m[96];
		/* 不换成「正在取消」:掐掉连接后阻塞调用几十毫秒内就带错误返回了,
		 * 为这一瞬间换一句话,看着反倒像又开始干别的活。只收起 B 的提示 */
		if (msg[0]) snprintf(m, sizeof(m), "%s%s%s",
		                     msg, dots[(osGetTime() / 300) % 4],
		                     cancelling ? "" : "   (B 取消)");
		else m[0] = 0;      /* 空消息 = 界面照常,状态条不占用 */
		busy_frame(m);
		if (net_is_shutting_down() || aptShouldClose()) break;
	}
	thread_reap(&th, 10000000000ULL, "bg job");
	return job.done ? job.ret : -1;
}

static void busy_frame(const char *msg) {
	snprintf(s_busy, sizeof(s_busy), "%s", msg);
	if (s_busy_minimal) {
		/* 两屏都要画:少画一屏的话,GPU 的两个后台缓冲各留着一份旧内容,
		 * 表现就是闪烁(这个坑在选集页和调试台那里已经踩过) */
		ui_begin();
		float tw = ui_text_width(msg, UI_SHARP);
		ui_text(200.0f - tw / 2.0f, 112, UI_SHARP, UI_COL_TEXT, msg);
		if (!ui_console_active()) {
			ui_begin_bottom();
			ui_text_clipped(10, 110, UI_SHARP, UI_COL_DIM, msg, 300);
		}
		ui_end();
		return;
	}
	draw_list();
	if (!ui_console_active()) {
		ListActions dummy = { 0 };
		draw_bottom_list(false, 0, 0, &dummy);
	}
	ui_end();
}

/* 播放器发弹幕时的登录回调:未登录则拉起扫码流程 */
static bool login_cb(void) {
	if (!bili_logged_in())
		do_login();
	/* 播放器内登录时若在登录页点了频道,这里没人消费会留成陈旧状态,
	 * 下次点历史/收藏就会莫名跳到那个频道(高亮也就"错位")→ 直接丢弃 */
	s_pending_mode = -1;
	return bili_logged_in();
}

/* 收藏夹必须在播放器建立长连接前读取。播放中再调账号 API 会与视频流
 * 共用 3DS httpc，旧实现偶发拿错响应，表现为列表闪现后播放器退出。 */
static void player_favorites_prepare(void) {
	if (!bili_logged_in()) {
		s_player_fav_n = 0;
		s_player_fav_loaded = false;
		player_set_favorite_folders(NULL, NULL, NULL, 0);
		return;
	}
	if (!s_player_fav_loaded) {
		if (run_bg(fav_folders_job, NULL, "读取收藏夹") == 0) {
			for (int i = 0; i < s_player_fav_n; i++) {
				s_player_fav_ids[i] = s_player_fav_folders[i].id;
				s_player_fav_titles[i] = s_player_fav_folders[i].title;
				s_player_fav_counts[i] = s_player_fav_folders[i].media_count;
			}
			s_player_fav_loaded = true;
		} else {
			s_player_fav_n = 0;
			s_player_fav_loaded = false; /* 下次播放重试，不缓存一次失败 */
		}
		s_busy[0] = 0;
	}
	player_set_favorite_folders(s_player_fav_ids, s_player_fav_titles,
	                            s_player_fav_counts, s_player_fav_n);
}

/* player_play 已经返回、NetStream 和 FFmpeg 都已关闭后才允许 POST。 */
static bool submit_player_favorite(BiliVideo *v) {
	int64_t folder_id = player_take_favorite_request();
	if (!folder_id) return false;
	if (!v || !bili_logged_in()) {
		snprintf(s_busy, sizeof(s_busy), "收藏失败:账号未登录");
		return true;
	}
	if (!v->aid) {
		s_job_video = v;
		if (run_bg(video_cid_job, NULL, "补充视频信息") != 0 || !v->aid) {
			snprintf(s_busy, sizeof(s_busy), "收藏失败:%s", bili_last_error());
			return true;
		}
	}
	const char *folder_name = "收藏夹";
	for (int i = 0; i < s_player_fav_n; i++)
		if (s_player_fav_ids[i] == folder_id) {
			folder_name = s_player_fav_titles[i];
			break;
		}
	s_job_fav_aid = v->aid;
	s_job_fav_id = folder_id;
	if (run_bg(fav_add_job, NULL, "提交收藏") == 0) {
		snprintf(s_busy, sizeof(s_busy), "已收藏到:%s", folder_name);
		/* 数量已变化，下次播放前重新读取，避免显示旧计数。 */
		s_player_fav_loaded = false;
	} else {
		snprintf(s_busy, sizeof(s_busy), "收藏失败:%s", bili_last_error());
	}
	return true;
}

/* 在线播放和本地播放都优先于后台下载。只有确认缓存 NetStream 已经关闭，
 * 才让播放器建立自己的输入，避免 3DS httpc 同时跑两条长流。 */
static bool cache_enter_foreground(const char *why) {
	download_worker_set_foreground(true);
	while (download_worker_is_active() && aptMainLoop()) {
		player_shutdown_timer_poll();
		if (net_is_shutting_down() || aptShouldClose()) return false;
		busy_frame(why ? why : "暂停后台缓存");
	}
	return !download_worker_is_active();
}

static void cache_leave_foreground(void) {
	if (!net_is_shutting_down() && !aptShouldClose())
		download_worker_set_foreground(false);
}

static void cache_duplicate_message(const char *bvid, int64_t cid,
                                    char *out, size_t outlen) {
	DownloadTask old;
	if (cache_manager_find(bvid, cid, &old) != 0) {
		snprintf(out, outlen, "该视频已有缓存任务，不会重复加入");
		return;
	}
	switch (old.status) {
	case DOWNLOAD_STATUS_COMPLETED:
		snprintf(out, outlen, "该视频已经缓存完成");
		break;
	case DOWNLOAD_STATUS_DOWNLOADING:
		snprintf(out, outlen, "该视频正在缓存，不会重复加入");
		break;
	case DOWNLOAD_STATUS_WAITING:
		snprintf(out, outlen, "该视频已在待缓存队列");
		break;
	case DOWNLOAD_STATUS_PAUSED:
		snprintf(out, outlen, "该视频已有暂停任务，请到离线下载任务继续");
		break;
	case DOWNLOAD_STATUS_FAILED:
		snprintf(out, outlen, "该视频已有失败任务，请到离线下载任务重试");
		break;
	default:
		snprintf(out, outlen, "该视频已有缓存任务，不会重复加入");
		break;
	}
}

static void cache_enqueue_message(const char *bvid, int64_t cid, int r,
                                  char *out, size_t outlen) {
	if (r == 0) {
		snprintf(out, outlen, "已加入缓存队列");
		download_worker_wake();
	} else if (r == 1) {
		cache_duplicate_message(bvid, cid, out, outlen);
	} else if (r == -2) {
		snprintf(out, outlen, "缓存队列已满(%d项)", CACHE_MAX_TASKS);
	} else if (r == -4) {
		snprintf(out, outlen, "同名文件冲突，请检查SD卡缓存目录");
	} else {
		snprintf(out, outlen, "加入缓存失败:%d", r);
	}
}

static void cache_part_title(const BiliVideo *v, const BiliPage *pg,
                             char *out, size_t outlen) {
	if (!pg) {
		snprintf(out, outlen, "%s", v->title);
	} else if (pg->title[0]) {
		snprintf(out, outlen, "%s - P%d %s", v->title, pg->page, pg->title);
	} else {
		snprintf(out, outlen, "%s - P%d", v->title, pg->page);
	}
}

static int cache_all_video_parts(BiliVideo *v, int qn,
                                 char *msg, size_t msglen) {
	if (s_npages <= 1) {
		const BiliPage *pg = s_npages == 1 ? &s_pages[0] : NULL;
		int64_t cid = pg ? pg->cid : v->cid;
		char title[200];
		cache_part_title(v, pg, title, sizeof(title));
		int r = cache_manager_enqueue(v->bvid, cid, v->aid, title, v->author, qn);
		cache_enqueue_message(v->bvid, cid, r, msg, msglen);
		return r;
	}

	for (int i = 0; i < s_npages; i++) {
		cache_part_title(v, &s_pages[i], s_batch_titles[i],
		                 sizeof(s_batch_titles[i]));
		s_batch_items[i] = (CacheEnqueueItem){
			v->bvid, s_pages[i].cid, v->aid, s_batch_titles[i],
			v->author, qn
		};
	}
	int added = 0, duplicates = 0;
	int r = cache_manager_enqueue_batch(s_batch_items, s_npages,
	                                    &added, &duplicates);
	if (added) download_worker_wake();
	if (r == -2)
		snprintf(msg, msglen, "已加入%dP，队列已满；已有%dP",
		         added, duplicates);
	else if (r != 0)
		snprintf(msg, msglen, "全集入队不完整:加入%dP 已有%dP 错误%d",
		         added, duplicates, r);
	else if (!added)
		snprintf(msg, msglen, "全集%dP已缓存或已在队列", duplicates);
	else
		snprintf(msg, msglen, "全集加入%dP，已有%dP不重复", added, duplicates);
	return r;
}

/* player.c 的触屏按钮回调。只改任务数据库并唤醒 worker；实际下载仍会
 * 等播放器退出、foreground 解除后才开始，避免与在线播放争用 httpc。 */
static int player_cache_cb(bool all_parts, char *msg, size_t msglen) {
	BiliVideo *v = s_play_cache_video;
	if (!v || !s_cache_ok || !s_play_cache_cid) {
		snprintf(msg, msglen, "当前视频不能加入缓存");
		return -1;
	}
	if (all_parts && s_npages > 1)
		return cache_all_video_parts(v, s_play_cache_qn, msg, msglen);

	const BiliPage *pg = NULL;
	for (int i = 0; i < s_npages; i++)
		if (s_pages[i].cid == s_play_cache_cid) { pg = &s_pages[i]; break; }
	char title[200];
	cache_part_title(v, pg, title, sizeof(title));
	int r = cache_manager_enqueue(v->bvid, s_play_cache_cid, v->aid,
	                              title, v->author, s_play_cache_qn);
	cache_enqueue_message(v->bvid, s_play_cache_cid, r, msg, msglen);
	return r;
}

static void cache_selected(void) {
	if (!s_net_ok) { load_list(); return; }
	if (!s_cache_ok) {
		snprintf(s_busy, sizeof(s_busy), "缓存目录/数据库不可写");
		return;
	}
	if (s_sel < 0 || s_sel >= s_count) return;
	BiliVideo *v = &s_list[s_sel];
	list_thumbs_stop();
	if (!v->cid) {
		set_status("获取视频信息...", "cache: fetching cid");
		busy_frame("获取视频信息");
		if (bili_get_cid(v->bvid, &v->cid, &v->aid) != 0) {
			snprintf(s_busy, sizeof(s_busy), "无法缓存:%s", bili_last_error());
			if (!net_is_shutting_down()) list_thumbs_start(s_sel);
			return;
		}
	}
	int r = cache_manager_enqueue(v->bvid, v->cid, v->aid, v->title,
	                              v->author, g_qn);
	cache_enqueue_message(v->bvid, v->cid, r, s_busy, sizeof(s_busy));
	if (!net_is_shutting_down()) list_thumbs_start(s_sel);
}

static void draw_cache_page(int sel, const char *notice) {
	int n = cache_manager_snapshot(s_cache_view, CACHE_MAX_TASKS);
	ui_begin();
	char head[48];
	snprintf(head, sizeof(head), "离线缓存  %d/%d", n ? sel + 1 : 0, n);
	ui_text(6, 3, 0.7f, UI_COL_DIM, head);
	if (!n) {
		ui_text(112, 104, UI_SHARP, UI_COL_DIM, "还没有缓存任务");
	} else {
		int first = sel - 3;
		if (first < 0) first = 0;
		if (first > n - 7) first = n - 7;
		if (first < 0) first = 0;
		for (int i = first; i < n && i < first + 7; i++) {
			float y = 30.0f + (float)(i - first) * 30.0f;
			if (i == sel) ui_rect_z(0, y - 1, 0.15f, 400, 29, UI_COL_SEL);
			ui_text_clipped(8, y, UI_SHARP, i == sel ? UI_COL_WHITE : UI_COL_TEXT,
			                s_cache_view[i].title, 270);
			char meta[96];
			if (s_cache_view[i].total_size) {
				unsigned long pct = (unsigned long)
					((s_cache_view[i].downloaded_size * 100u) /
					 s_cache_view[i].total_size);
				snprintf(meta, sizeof(meta), "%s %lu%%",
				         cache_manager_status_name(s_cache_view[i].status), pct);
			} else {
				snprintf(meta, sizeof(meta), "%s",
				         cache_manager_status_name(s_cache_view[i].status));
			}
			ui_text_clipped(286, y, 0.55f, UI_COL_DIM, meta, 108);
		}
	}

	ui_begin_bottom();
	ui_text(10, 8, UI_SHARP, UI_COL_TEXT, "缓存管理");
	if (n) {
		ui_text_clipped(10, 38, UI_SHARP, UI_COL_WHITE,
		                s_cache_view[sel].title, 300);
		ui_text_clipped(10, 66, 0.7f, UI_COL_DIM,
		                s_cache_view[sel].author, 300);
	}
	ui_rect(10, 112, 300, 48, C2D_Color32(0x26, 0x26, 0x30, 0xFF));
	ui_text(18, 120, 0.65f, UI_COL_WHITE,
	        "A 离线播放   X 暂停/继续/重试");
	ui_text(18, 140, 0.65f, UI_COL_WHITE,
	        "Y 删除       B 返回");
	char worker[128];
	download_worker_status(worker, sizeof(worker));
	const char *line = notice && notice[0] ? notice : worker;
	ui_rect(10, 174, 300, 52, C2D_Color32(0x1D, 0x1D, 0x26, 0xFF));
	ui_text_clipped(18, 184, 0.6f, UI_COL_DIM,
	                line && line[0] ? line : "后台队列空闲", 284);
	ui_end();
}

static void cache_list_page(void) {
	int sel = 0;
	char notice[128] = "";
	while (aptMainLoop()) {
		player_shutdown_timer_poll();
		hidScanInput();
		u32 kd = hidKeysDown();
		int n = cache_manager_snapshot(s_cache_view, CACHE_MAX_TASKS);
		if (sel >= n) sel = n > 0 ? n - 1 : 0;
		if ((kd & KEY_UP) && sel > 0) sel--;
		if ((kd & KEY_DOWN) && sel + 1 < n) sel++;
		if (kd & KEY_B) break;
		if (n && (kd & KEY_X)) {
			DownloadTask *t = &s_cache_view[sel];
			if (t->status == DOWNLOAD_STATUS_WAITING ||
			    t->status == DOWNLOAD_STATUS_DOWNLOADING) {
				cache_manager_pause(t);
				download_worker_cancel_current();
				snprintf(notice, sizeof(notice), "任务已暂停");
			} else if (t->status == DOWNLOAD_STATUS_PAUSED) {
				cache_manager_resume(t); download_worker_wake();
				snprintf(notice, sizeof(notice), "任务已继续");
			} else if (t->status == DOWNLOAD_STATUS_FAILED) {
				cache_manager_retry(t); download_worker_wake();
				snprintf(notice, sizeof(notice), "任务已重新排队");
			}
		}
		else if (n && (kd & KEY_Y)) {
			DownloadTask t = s_cache_view[sel];
			/* 状态先改成 PAUSED，再让 worker 退出并关闭 FILE。不能只看状态就
			 * remove(.part)：线程可能刚通过一次 interrupted()，仍在写最后一块。 */
			if (t.status == DOWNLOAD_STATUS_DOWNLOADING)
				cache_manager_pause(&t);
			if (cache_enter_foreground("停止后台缓存")) {
				int r = cache_manager_remove(&t);
				if (r == 0) snprintf(notice, sizeof(notice), "缓存已删除");
				else snprintf(notice, sizeof(notice), "删除失败:%d", r);
				cache_leave_foreground();
			}
		}
		else if (n && (kd & KEY_A)) {
			DownloadTask t = s_cache_view[sel];
			if (t.status != DOWNLOAD_STATUS_COMPLETED) {
				snprintf(notice, sizeof(notice), "下载完成后才能离线播放");
			} else if (cache_enter_foreground("准备离线播放")) {
				dm_free(); sub_free();
				player_set_pages(NULL, NULL, 0, 0);
				player_set_cache_cb(NULL); /* 已经是缓存文件，不再显示重复缓存入口 */
				player_set_meta(0, 0, "");
				player_set_prefs(false, g_force_sw, t.qn);
				player_play_file(t.filepath, t.title);
				cache_leave_foreground();
				notice[0] = 0;
			}
		}
		if (net_is_shutting_down() || aptShouldClose()) break;
		draw_cache_page(sel, notice);
	}
}

/* 真正的“合集”是 ugc_season（跨多个 bvid），不是单个稿件的分 P。
 * bili_collection() 已经逐页取全；这里展示的条目数就是缓存完整合集时
 * 将要处理的真实数量。 */
static void collection_page(BiliVideo *origin) {
	if (!s_net_ok || !origin || !origin->bvid[0]) return;
	if (!cache_enter_foreground("暂停后台缓存")) return;
	list_thumbs_stop();
	s_collection_n = 0;
	s_job_bvid = origin->bvid;
	if (run_bg(collection_job, NULL, "读取完整合集") != 0 ||
	    s_collection_n <= 0) {
		snprintf(s_busy, sizeof(s_busy), "读取合集失败:%s", bili_last_error());
		if (!net_is_shutting_down() && s_count > 0) list_thumbs_start(s_sel);
		cache_leave_foreground();
		return;
	}

	int sel = 0;
	for (int i = 0; i < s_collection_n; i++)
		if (!strcmp(s_collection_videos[i].bvid, origin->bvid)) { sel = i; break; }
	char notice[160] = "";
	while (aptMainLoop()) {
		player_shutdown_timer_poll();
		hidScanInput();
		u32 kd = hidKeysDown();
		touchPosition tp = { 0, 0 };
		bool touched = (kd & KEY_TOUCH) != 0;
		if (touched) hidTouchRead(&tp);
		if ((kd & KEY_UP) && sel > 0) sel--;
		if ((kd & KEY_DOWN) && sel + 1 < s_collection_n) sel++;

		ui_begin();
		int first = sel - 3;
		if (first < 0) first = 0;
		if (first > s_collection_n - 7) first = s_collection_n - 7;
		if (first < 0) first = 0;
		for (int i = first; i < s_collection_n && i < first + 7; i++) {
			float y = 4.0f + (float)(i - first) * 33.0f;
			if (i == sel) ui_rect_z(0, y, 0.15f, 400, 31, UI_COL_SEL);
			ui_text_clipped(8, y + 5, UI_SHARP,
			                i == sel ? UI_COL_WHITE : UI_COL_TEXT,
			                s_collection_videos[i].title, 326);
			if (s_collection_videos[i].duration > 0) {
				char db[16];
				int d = s_collection_videos[i].duration;
				snprintf(db, sizeof(db), "%d:%02d", d / 60, d % 60);
				ui_text(344, y + 5, 0.65f, UI_COL_DIM, db);
			}
		}

		ui_begin_bottom();
		char title[240];
		snprintf(title, sizeof(title), "%s  (%d个视频)",
		         s_collection_info.title, s_collection_n);
		ui_text_clipped(10, 6, UI_SHARP, UI_COL_TEXT, title, 300);
		bool all = ui_button(10, 38, 300, 40, "缓存完整合集", UI_COL_SEL,
		                     touched, tp.px, tp.py);
		bool play = ui_button(10, 86, 145, 40, "播放本视频", UI_COL_SEL,
		                      touched, tp.px, tp.py);
		bool one = ui_button(165, 86, 145, 40, "缓存本视频", UI_COL_SEL,
		                     touched, tp.px, tp.py);
		bool tasks = ui_button(10, 134, 145, 40, "下载任务", UI_COL_SEL,
		                       touched, tp.px, tp.py);
		bool back = ui_button(165, 134, 145, 40, "返回", UI_COL_SEL,
		                      touched, tp.px, tp.py);
		ui_rect(10, 184, 300, 46, C2D_Color32(0x26, 0x26, 0x30, 0xFF));
		ui_text_clipped(18, 194, 0.62f, UI_COL_DIM,
		                notice[0] ? notice : "↑↓选择  A播放  B返回", 284);
		ui_end();

		if (back || (kd & KEY_B)) break;
		if (all) {
			s_collection_resolve_at = -1;
			if (run_bg(collection_resolve_job, NULL, "读取合集视频信息") != 0) {
				int bad = s_collection_resolve_at;
				snprintf(notice, sizeof(notice), "第%d项信息读取失败:%s",
				         bad >= 0 ? bad + 1 : 0, bili_last_error());
			} else {
				for (int i = 0; i < s_collection_n; i++)
					s_batch_items[i] = (CacheEnqueueItem){
						s_collection_videos[i].bvid,
						s_collection_videos[i].cid,
						s_collection_videos[i].aid,
						s_collection_videos[i].title,
						s_collection_videos[i].author,
						g_qn
					};
				int added = 0, duplicates = 0;
				int r = cache_manager_enqueue_batch(s_batch_items, s_collection_n,
				                                    &added, &duplicates);
				if (added) download_worker_wake();
				if (r == -2)
					snprintf(notice, sizeof(notice),
					         "加入%d个，队列已满；已有%d个", added, duplicates);
				else if (r != 0)
					snprintf(notice, sizeof(notice),
					         "合集入队不完整:加入%d 已有%d 错误%d",
					         added, duplicates, r);
				else if (!added)
					snprintf(notice, sizeof(notice),
					         "完整合集%d个均已缓存或在队列", duplicates);
				else
					snprintf(notice, sizeof(notice),
					         "完整合集加入%d个，已有%d个不重复", added, duplicates);
			}
		}
		if (one) {
			BiliVideo *v = &s_collection_videos[sel];
			if (!v->cid) {
				s_job_video = v;
				if (run_bg(video_cid_job, NULL, "读取视频信息") != 0) {
					snprintf(notice, sizeof(notice), "无法缓存:%s", bili_last_error());
					continue;
				}
			}
			int r = cache_manager_enqueue(v->bvid, v->cid, v->aid,
			                              v->title, v->author, g_qn);
			cache_enqueue_message(v->bvid, v->cid, r, notice, sizeof(notice));
		}
		if (tasks) cache_list_page();
		if (play || (kd & KEY_A)) {
			play_video(&s_collection_videos[sel]);
			list_thumbs_stop();
		}
		if (net_is_shutting_down() || aptShouldClose()) break;
	}
	if (!net_is_shutting_down() && s_count > 0) list_thumbs_start(s_sel);
	cache_leave_foreground();
}

/* 分P 一行的显示文本:"P3  标题"(没标题就只有 "P3") */
static void page_label(const BiliPage *pg, char *out, size_t n) {
	if (pg->title[0]) snprintf(out, n, "P%d  %s", pg->page, pg->title);
	else              snprintf(out, n, "P%d", pg->page);
}

/* 【选集页已移进播放器】原来这里有一个 choose_page():开播前的独立一页。
 * 改掉的原因是它决定了上屏能显示什么 —— 跑到那一页时播放器已经退出、
 * 纹理已经释放,上屏没有画面可留,只能另画一套标题+时长的排版。
 * 而「换一集」这个动作本来就发生在看片当中,上屏理应停在暂停的那一帧。
 *
 * 现在它是播放器里的一个子页面(和评论区同构):上屏保持暂停,
 * 下屏整个换成列表。播放器仍然不碰分P 数据 —— 标签和时长从这里传进去,
 * 选中后只回一个下标。
 *
 * 教训:界面归属不该只看「代码放哪儿更整齐」,还要看**它需要什么上下文**。
 * 这一页需要的是「视频还在、只是停住了」,那它就只能待在播放器里。 */


/* 真正开播一条流。cid 由调用方给 —— 多 P 视频必须传**选中那一 P**的 cid,
 * 弹幕、字幕、进度上报全按 cid 走,只有它换对了才是真的换了一集。 */
static void play_stream(BiliVideo *v, int64_t cid, const char *disp_title) {
	set_status("解析播放地址...", "resolving play url...");
	busy_frame("解析播放地址...");
	ui_trace("resolving playurl qn=%d", g_qn);
	char url[2048];
	/* 弹幕先起跑:取流那几个 API 往返是等延迟、不吃带宽的,
	 * 这段时间正好给弹幕下载用,能省下一两秒 */
	if (g_danmaku)
		dm_load_async(cid);
	/* 字幕不在这里拉:此刻正与 取流/弹幕 并发,3DS httpc 在多路并发下
	 * 会把响应张冠李戴(实测:字幕内容是别的视频的)。改为播放真正开始、
	 * 其它请求都收摊之后,由播放器单独去拉(player.c 里 sub_kicked) */
	sub_free();   /* 先清干净,杜绝上一个视频的残留 */
	int used_qn = g_qn;
	s_job_bvid = v->bvid; s_job_cid = cid; s_job_url = url;
	s_job_qn = g_qn;
	int r = run_bg(playurl_job, NULL, "解析播放地址");
	/* 带上原因:光看 r=-1 分不出「没发请求」「请求失败」「接口拒绝」,
	 * 而这三者的修法完全不同 */
	ui_trace("playurl r=%d err=%s", r, bili_last_error());
	if (r != 0 && g_qn != QN_360) { /* 拿不到就回落 360P(最通用的一档) */
		printf("qn=%d failed, fallback to 360P\n", g_qn);
		s_job_qn = QN_360;
		r = run_bg(playurl_job, NULL, "解析播放地址(360P)");
		used_qn = 16;
	}
	if (r != 0 && s_job_cancelled) {
		/* 用户按 B 掐掉的:当作没点过这个视频,别弹错误 */
		set_status("", "playurl cancelled");
		s_busy[0] = 0;
		dm_free();
		sub_free();
		return;
	}
	if (r != 0) {
		{
			const char *why = bili_last_error();
			char msg[128];
			snprintf(msg, sizeof(msg), "取流失败:%s",
			         (why && why[0]) ? why : "可能需登录或视频受限");
			set_status(msg, "playurl failed");
			snprintf(s_busy, sizeof(s_busy), "%s", msg);
		}
		dm_free();   /* 弹幕已经起跑了,不播就得把线程收掉 */
		sub_free();
		return;
	}
	player_set_meta(v->aid, cid, v->bvid);
	/* 【每次开播都从存档重读弹幕开关】播放页里也能开关弹幕,而那个改动
	 * 落在 player.c 的静态变量上,main.c 的 g_danmaku 并不知道。
	 * 不重读的话,这里会拿一个过期的值把用户刚才的选择覆盖掉。
	 * 存档是两处共同的真相来源,以它为准。 */
	g_danmaku = settings_get("danmaku", g_danmaku ? 1 : 0) != 0;
	player_set_prefs(g_danmaku, g_force_sw, used_qn);
	s_play_cache_video = v;
	s_play_cache_cid = cid;
	s_play_cache_qn = used_qn;
	player_set_cache_cb(player_cache_cb);
	s_busy[0] = 0;
	player_play(url, disp_title);
	player_set_cache_cb(NULL);
	s_play_cache_video = NULL;
	s_play_cache_cid = 0;
	/* 【回列表前把状态条擦掉】播放器开流时借 busy_frame 写过「连接中」
	 * 「载入视频」,那几个字一直留在 s_busy 里 —— 播放期间被视频盖着看
	 * 不见,退出来才露出来,于是列表底下挂着一句早就做完的事。
	 * 状态条写的是"正在做什么",没在做就该是空的。 */
	s_busy[0] = 0;
	ui_trace_sync("exit-path: dm_free");
	dm_free();
	ui_trace_sync("exit-path: sub_free");
	sub_free();
}

static void play_video(BiliVideo *v) {
	if (!s_net_ok) { load_list(); return; }
	if (!v || !v->bvid[0]) return;
	/* 清掉上一次异常退出可能遗留的收藏请求。 */
	(void)player_take_favorite_request();

	/* 第一件事:停掉封面下载线程。
	 * 它们是唯一绕过 net 串行锁的请求(net_get_img),留着的话
	 * 接下来的 取cid/字幕/弹幕/取流 会与两条封面线程五路并发,
	 * 3DS httpc 在这种压力下会把响应张冠李戴 —— 实测症状就是
	 * "第一次进某视频字幕是别的视频的,退出等一会儿再进就正常"
	 * (等的那会儿封面刚好下完,并发消失) */
	list_thumbs_stop();
	player_favorites_prepare();

	if (v->cid == 0) {
		set_status("获取视频信息...", "fetching cid...");
		busy_frame("获取视频信息...");
		if (bili_get_cid(v->bvid, &v->cid, &v->aid) != 0) {
			/* 注意:不要在这里自动移除条目!曾经这么干过,结果正常视频
			 * 也被误杀(接口对 3DS 的请求风控性 404,视频本身是好的)。
			 * 只显示原因,让用户自己决定 */
			const char *why = bili_last_error();
			char msg[128];
			snprintf(msg, sizeof(msg), "无法播放:%s",
			         (why && why[0]) ? why : "获取 cid 失败");
			set_status(msg, "get cid failed (see console)");
			snprintf(s_busy, sizeof(s_busy), "%s", msg);  /* 状态条留原因 */
			return;
		}
	}
	/* ---- 分 P ----
	 * 只在「可能不止一 P」时才发这个请求:3DS 上一次网络往返几百毫秒,
	 * 而绝大多数视频就一 P。热门/历史/收藏的列表里已经带了分 P 数
	 * (videos / page 字段),推荐和搜索没带 —— 那两条路要多问一次,
	 * 但问完就记在 v->pages 里,同一个视频再进来不会再问。 */
	s_npages = 0;
	if (v->pages != 1) {
		s_job_bvid = v->bvid;
		if (run_bg(pagelist_job, NULL, "检查分P") != 0)
			s_npages = 0;
		if (s_npages > 0) v->pages = s_npages;
		s_busy[0] = 0;
	}

	if (s_npages > 1) {
		/* 选集列表交给播放器画(上屏要保持暂停的画面,所以它必须在
		 * 播放器**里面**)。这两个数组是文件作用域的,播放期间一直有效 ——
		 * 播放器只读不存,别改成栈上的。 */
		for (int i = 0; i < s_npages; i++) {
			page_label(&s_pages[i], s_pg_label[i], sizeof(s_pg_label[i]));
			s_pg_labelp[i] = s_pg_label[i];
			s_pg_dur[i] = s_pages[i].duration;
		}
	}

	if (s_npages > 1) {
		/* 【直接播,不先问】多 P 视频进来就放第一 P(或列表里那个 cid
		 * 对得上的一 P)。开播前横插一个选集页,对「点进去就想看」这个
		 * 最常见的意图是纯粹的摩擦 —— 而绝大多数人点进合集就是从头看。
		 * 想换 P 的,播放中下屏左下角有「选集」按钮。 */
		int cur = 0;
		for (int i = 0; i < s_npages; i++)
			if (s_pages[i].cid == v->cid) { cur = i; break; }
		for (;;) {
			char t[220];
			if (s_pages[cur].title[0])
				snprintf(t, sizeof(t), "P%d %s | %s",
				         s_pages[cur].page, s_pages[cur].title, v->title);
			else
				snprintf(t, sizeof(t), "P%d | %s",
				         s_pages[cur].page, v->title);
			player_set_pages(s_pg_labelp, s_pg_dur, s_npages, cur);
			play_stream(v, s_pages[cur].cid, t);
			if (submit_player_favorite(v)) break;
			/* 【只有在选集里挑了才继续】按 B 退出播放器是「我看完了」,
			 * 不是「我要挑下一集」—— 以前播完无条件弹选集页,
			 * 想走的人得按两次 B。 */
			int pick = player_take_page_pick();
			/* 【自动连播下一 P】看完一集自动接下一集,是合集/课程最自然的
			 * 期待。但只在**自然播到片尾**时才接 —— 按 B 退出是「我不看了」,
			 * 那时候把人带到下一集比不接更糟。 */
			if (pick < 0 && player_ended_naturally() && cur + 1 < s_npages) {
				pick = cur + 1;
				ui_trace("自动连播:P%d → P%d", cur + 1, pick + 1);
			}
			if (pick < 0) break;
			/* 系统要关我们:别再开下一段流了 */
			if (net_is_shutting_down() || aptShouldClose()) break;
			cur = pick;
			/* 从第二段起就是「换一集」而不是「开始看」,取流期间
			 * 上屏别退回列表页 */
			s_busy_minimal = true;
		}
		s_busy_minimal = false;
	} else {
		play_stream(v, v->cid, v->title);
		(void)submit_player_favorite(v);
	}
	player_set_pages(NULL, NULL, 0, 0);

	/* 【系统正在关我们时别再启动封面下载】否则刚被掐掉的两个 loader 线程
	 * 立刻又被拉起来,接着 thumb_exit 还得把它们收一遍 —— 白白拖长
	 * "Closing software"。 */
	if (s_count > 0 && !net_is_shutting_down()) list_thumbs_start(s_sel);
	ui_trace_sync("exit-path: play_video done");
}

static void play_selected(void) {
	if (s_sel < 0 || s_sel >= s_count) return;
	BiliVideo *v = &s_list[s_sel];
	play_video(v);
}

/* APT 事件回调(HOME/睡眠/退出)。只做置标志这类轻活,别在回调里干重活 */
static void apt_hook_cb(APT_HookType hook, void *param) {
	(void)param;
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
		player_notify_suspend();
		download_worker_notify_suspend(true);
		/* 封面下载也停一下并掐掉在途那个,否则按 HOME 要等它跑完 */
		thumb_notify_suspend(1);
	} else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
		thumb_notify_suspend(0);
		download_worker_notify_suspend(false);
	} else if (hook == APTHOOK_ONEXIT) {
		/* 【HOME → X 卡在 "Closing software" 的解法】
		 * 系统要关掉我们,而 aptMainLoop() 只有主循环转起来才会返回 false。
		 * 主线程要是正卡在一个同步 HTTP 请求里(列表、播放地址都是主线程
		 * 发的,httpc 没有超时),主循环就回不来,系统只能一直等。
		 * 所以在这里把网络整个掐掉并封死:在途的立刻失败,之后的一律拒绝,
		 * 主线程于是很快回到循环、看到 false、走正常清理。
		 * 这一步不可逆 —— 无所谓,ONEXIT 之后不会再回来了。 */
		net_shutdown_begin();
		download_worker_notify_suspend(true);
		thumb_notify_suspend(1);
		player_notify_suspend();
	}
}

int main(void) {
	/* 界面本来就该跑在满速上,不必等到播视频才提。Old3DS 上是空操作。
	 *
	 * 【别顺手在这儿加 APT_SetAppCpuTimeLimit】曾经加过,理由是「启动后
	 * 到第一次播放之前界面很卡」。后来实测证明**那个理由是错的** ——
	 * 用固定计算量了一把,播放前后耗时完全一样,主频从头到尾没变过。
	 * 真凶是封面缓存扫描线程在猛敲文件系统(见 thumb.c)。
	 * 而 SetAppCpuTimeLimit 是给「应用要用系统核」用的,常驻着反而可能
	 * 挤占 HOME 菜单所在的那个核心 —— 它留在 play_stream 里就够了。 */
	osSetSpeedupEnable(true);
	ui_init();
	/* HOME/睡眠钩子:挂起时暂停播放。不挂这个钩子的话,按 HOME 后
	 * 下载和解码线程照跑,HOME 菜单卡、回来更卡 */
	{
		static aptHookCookie s_apt_cookie;
		aptHook(&s_apt_cookie, apt_hook_cb, NULL);
	}
	/* 线性内存总量在这里报一次。MVD 工作缓冲、视频输出、音频缓冲全从这里出,
	 * "linear alloc failed" 时先回头看这个数就知道是天生不够还是被吃光了。
	 * 注:.3dsx 走 Homebrew Launcher 拿到的内存通常比装成 .cia 少不少 */
	printf("linear heap free at boot: %luKB\n",
	       (unsigned long)(linearSpaceFree() / 1024));
	/* 普通堆探针:从 16MB 往下折半试 malloc,量出最大可分配块。
	 * "字体 8MB 装不装得下、ffmpeg 够不够用"直接看这个数 */
	{
		size_t probe = 16 * 1024 * 1024;
		void *pp = NULL;
		while (probe >= 64 * 1024 && !(pp = malloc(probe))) probe /= 2;
		if (pp) free(pp);
		ui_trace("boot: linear=%luKB heap-maxblock=%luKB",
		         (unsigned long)(linearSpaceFree() / 1024),
		         (unsigned long)(pp ? probe / 1024 : 0));
	}
	ime_init();   /* 加载拼音词库(约 1MB,失败则输入退化为英文) */
	player_set_login_cb(login_cb);
	/* 开流阶段沿用列表页那一屏,状态写在状态条上(见 player.h) */
	player_set_busy_cb(busy_frame);

	/* 机型默认:New3DS 480P(有 MVD 硬解),老 3DS 360P(只能软解) */
	{
		bool n3 = false;
		APT_CheckNew3DS(&n3);
		g_qn = n3 ? QN_480 : QN_360;
	}
	/* 存档覆盖默认值。顺序要在机型判断**之后**:清晰度的默认因机型而异,
	 * settings_get 的 def 参数要拿到正确的机型默认 */
	settings_init();
	g_danmaku = settings_get("danmaku", g_danmaku ? 1 : 0) != 0;
	{	/* 存档里可能留着已经撤掉的 240P(qn=6),按无效处理回落默认 */
		int v = settings_get("qn", g_qn);
		g_qn = (v == QN_480 || v == QN_360) ? v : g_qn;
	}
	g_force_sw = settings_get("force_sw", g_force_sw ? 1 : 0) != 0;
	player_prefs_init();   /* 字幕开关/弹幕字号/字幕字号(存在 player.c) */

	printf("bilibili\ninit network...\n");
	s_net_ok = R_SUCCEEDED(net_init());
	if (!s_net_ok) {
		printf("httpc init failed\n");
	} else {
		bili_init();
		printf("%s\n", bili_logged_in() ? "logged in" : "not logged in (press X)");
	}
	s_cache_ok = cache_manager_init() == 0;
	if (!s_cache_ok)
		ui_trace("cache manager init failed");
	if (s_net_ok && s_cache_ok && download_worker_start() != 0)
		ui_trace("cache worker start failed");

	load_list();
	ui_bottom_debug(false); /* 启动完成,切到操作提示面板 */


	while (aptMainLoop()) {
		player_shutdown_timer_poll();
		hidScanInput();
		u32 kDown = hidKeysDown();
		(void)0; /* kRepeat 已弃用:上下键改为 单击+长按连续滚动 */
		touchPosition tp = { 0, 0 };
		bool touched = (kDown & KEY_TOUCH) != 0;
		if (touched) hidTouchRead(&tp);

		/* 日志页只用触摸:拖动滚动、双击退出(都在 ui_draw_log 里)。
		 * 方向键/摇杆一律不接管 —— 它们在列表页是选视频用的,
		 * 让同一个键在两个场景做不同的事只会误触 */

		if (kDown & KEY_B) {
			if (s_in_settings) {
				s_in_settings = false;
			} else if (s_mode != MODE_RECOMMEND) {
				/* 从搜索/热门/历史/收藏(含失败)返回默认的推荐页 */
				s_mode = MODE_RECOMMEND;
				s_hl_mode = -1;
				s_page = 1;
				load_list();
			}
		}
		if ((kDown & KEY_START) &&
		    !(hidKeysHeld() & KEY_SELECT)) /* START+SELECT 是强制软解组合键 */
			break;
		if (!ui_console_active())
		{	/* 选择与滚动。
			 * 十字键:单击移一行(目标缓动滑入);按住超过 300ms 进入
			 * "连续滚动"——视口匀速走、选中项贴边跟随,而不是逐行连发。
			 * 逐行连发即使做了缓动也是节奏性的"平滑跳",体验仍是跳。
			 * 调试台开着时整块跳过:那会儿方向键/摇杆不该动任何东西,
			 * 日志页只用触摸(拖动滚动、双击退出) */
			static u64 hold_t0 = 0;
			if (kDown & KEY_DOWN && s_sel + 1 < s_count) {
				s_sel++;
				hold_t0 = osGetTime();
				float bot = (s_sel + 1) * ROW_H;
				if (bot > s_scroll_t + LIST_H) s_scroll_t = bot - LIST_H;
			}
			if (kDown & KEY_UP && s_sel > 0) {
				s_sel--;
				hold_t0 = osGetTime();
				float top = s_sel * ROW_H;
				if (top < s_scroll_t) s_scroll_t = top;
			}
			u32 held = hidKeysHeld();
			bool long_hold = (held & (KEY_UP | KEY_DOWN)) &&
			                 osGetTime() - hold_t0 > 300;
			if (long_hold && (held & KEY_DOWN)) {
				s_scroll_t += 4.5f;               /* 匀速下滚 */
				int last = (int)((s_scroll_t + LIST_H) / ROW_H) - 1;
				if (last >= s_count) last = s_count - 1;
				if (last > s_sel) s_sel = last;   /* 选中项贴下边缘跟随 */
			}
			if (long_hold && (held & KEY_UP)) {
				s_scroll_t -= 4.5f;               /* 匀速上滚 */
				int firstv = (int)((s_scroll_t + ROW_H - 1) / ROW_H);
				if (firstv < 0) firstv = 0;
				if (firstv < s_sel) s_sel = firstv;  /* 贴上边缘跟随 */
			}
			/* 左摇杆:匀速滚动,不动选中项 */
			circlePosition cp;
			hidCircleRead(&cp);
			if (cp.dy > 24 || cp.dy < -24)
				s_scroll_t -= (float)cp.dy * 0.055f;
			/* 夹取 + 缓动 */
			float maxs = s_count * ROW_H - LIST_H;
			if (maxs < 0) maxs = 0;
			if (s_scroll_t < 0) s_scroll_t = 0;
			if (s_scroll_t > maxs) s_scroll_t = maxs;
			s_scroll += (s_scroll_t - s_scroll) * 0.35f;
			if (s_scroll - s_scroll_t < 0.5f && s_scroll_t - s_scroll < 0.5f)
				s_scroll = s_scroll_t;

			int first_visible = (int)(s_scroll_t / ROW_H);
			if (first_visible < 0) first_visible = 0;
			if (!s_in_settings) list_thumbs_ensure(first_visible);

			/* 推荐/热门/历史改为连续流：视口接近尾部就追加下一页，
			 * 原有条目、选中项和滚动位置全部保留。 */
			bool endless = s_mode == MODE_RECOMMEND ||
			               s_mode == MODE_POPULAR || s_mode == MODE_HISTORY ||
			               s_mode == MODE_FAV;
			int last_visible = (int)((s_scroll_t + LIST_H) / ROW_H);
			if (!s_in_settings && !(kDown & KEY_R) && endless &&
			    s_count > 0 && s_count < MAX_LIST &&
			    (!s_page_end || s_page < s_page_end) &&
			    last_visible >= s_count - 3) {
				s_page++;
				load_list();
			}
		}
		if (kDown & KEY_R) {
			if (s_page_end && s_page >= s_page_end) {
				/* 已经知道到底了:不再发那次注定为空的请求。
				 * 提示照给 —— 按了没反应比按了说"到底了"更让人困惑。 */
				snprintf(s_busy, sizeof(s_busy), "已经是最后一页");
			} else {
				s_page++;
				load_list();
			}
		}
		if (kDown & KEY_L) {
			/* 连续列表不再“返回上一页并覆盖内容”；L 改为向上翻一屏。 */
			s_scroll_t -= LIST_H;
			if (s_scroll_t < 0) s_scroll_t = 0;
			int firstv = (int)(s_scroll_t / ROW_H);
			if (s_sel > firstv) s_sel = firstv;
		}
		if ((kDown & (KEY_ZL | KEY_ZR)) ||
		    ((kDown & KEY_SELECT) && !(hidKeysHeld() & KEY_START))) {
			/* 频道循环只含 热门↔推荐;历史/收藏走下屏独立按钮。
			 * SELECT 与 ZL/ZR 同功能,照顾没有 ZL/ZR 的老机型 */
			s_mode = (s_mode == MODE_POPULAR) ? MODE_RECOMMEND : MODE_POPULAR;
			s_hl_mode = -1;
			s_page = 1;
			load_list();
		}
		if (kDown & KEY_Y) do_search();
		if (kDown & KEY_A) {
			if (s_count == 0) {   /* 列表为空(如启动时没网):A = 重试 */
				printf("retrying...\n");
				if (!s_net_ok) {
					s_net_ok = R_SUCCEEDED(net_init());
					if (s_net_ok) {
						bili_init();
						if (s_cache_ok && download_worker_start() != 0)
							ui_trace("cache worker retry start failed");
					}
				} else {
					bili_init();
				}
				load_list();
			} else {
				if (cache_enter_foreground("暂停后台缓存")) play_selected();
				cache_leave_foreground();
				/* 【播放中 HOME→X 卡死的最后一块】播放器是从主循环这一格
				 * 里调进去的。被系统关闭时,播放器自己退得很干净(实测
				 * 全链路 150ms),但返回后**本轮循环还剩大半格没走完** ——
				 * 接下来会照常画一帧列表页,ui_end 里要等 VBlank,而退出
				 * 状态下 GSP 的 VBlank 事件可能永远不来,主线程就吊死在
				 * 那里,永远轮不到下一次 aptMainLoop() 返回 false。
				 * (日志指纹:最后一行是 exit-path: play_video done,
				 * 而 exit: thumb_exit 永远没出现。)
				 * 所以这里直接跳出主循环,别再画这最后一帧。 */
				if (net_is_shutting_down() || aptShouldClose()) {
					/* aptShouldClose 直接问 APT,不吃钩子竞态的亏 */
					net_shutdown_begin();
					ui_trace_sync("exit: bail after playback");
					break;
				}
			}
		}

		/* 同样的兜底给搜索/登录:它们内部也各有一个 aptMainLoop 子循环,
		 * 被关闭时从里面返回后,剩下的这半格循环同样不能再画帧。 */
		if (net_is_shutting_down() || aptShouldClose()) {
			net_shutdown_begin();
			ui_trace_sync("exit: bail before draw");
			break;
		}

		if (s_in_settings) {
			ui_begin();
			/* 松手帧 hidTouchRead 拿不到坐标,而点选正是在松手时判定的 ——
			 * 这里把「按住」的坐标一路带进去,由列表自己记住最后一次 */
			bool held_t = (hidKeysHeld() & KEY_TOUCH) != 0;
			bool up_t = (hidKeysUp() & KEY_TOUCH) != 0;
			touchPosition tps = { 0, 0 };
			if (held_t) hidTouchRead(&tps);
			draw_settings(touched, held_t, up_t, tps.px, tps.py);
			if (ui_console_active()) {
				if (ui_draw_log(touched, held_t, tps.px, tps.py)) {
					s_debug_ui = false;
					ui_bottom_debug(false);
				}
			}
			ui_end();
			if (s_debug_ui && !ui_console_active())
				ui_bottom_debug(true);
			if (kDown & KEY_B) s_in_settings = false;
			continue;
		}

		ListActions act = { 0 };
		draw_list();
		if (ui_console_active()) {
			touchPosition th;
			hidTouchRead(&th);
			bool held = (hidKeysHeld() & KEY_TOUCH) != 0;
			if (ui_draw_log(touched, held, th.px, th.py)) {
				s_debug_ui = false;
				ui_bottom_debug(false);
			}
		} else {
			draw_bottom_list(touched, tp.px, tp.py, &act);
		}
		ui_end();

		if (act.login) {
			do_login();
			if (s_pending_mode >= 0) {
				s_mode = (ListMode)s_pending_mode;
				s_pending_mode = -1;
				s_page = 1;
				load_list();
			}
		}
		if (act.search) do_search();
		if (act.settings) { s_in_settings = true; ui_list_reset(); }
		if (act.cache) cache_selected();
		if (act.parts && s_sel >= 0 && s_sel < s_count)
			collection_page(&s_list[s_sel]);
		if (act.downloads) {
			list_thumbs_stop();
			cache_list_page();
			if (!net_is_shutting_down() && s_count > 0) list_thumbs_start(s_sel);
		}
		if (act.popular) {
			s_hl_mode = -1;
			if (s_mode != MODE_POPULAR) { s_mode = MODE_POPULAR; s_page = 1; load_list(); }
		}
		if (act.recommend) {
			s_hl_mode = -1;
			if (s_mode != MODE_RECOMMEND) { s_mode = MODE_RECOMMEND; s_page = 1; load_list(); }
		}
		if (act.hist || act.fav) {
			ListMode want = act.hist ? MODE_HISTORY : MODE_FAV;
			if (!bili_logged_in())
				do_login();          /* 未登录:直接拉起扫码 */
			if (s_pending_mode >= 0) {           /* 登录界面里改去别的频道 */
				s_mode = (ListMode)s_pending_mode;
				s_pending_mode = -1;
				s_page = 1;
				load_list();
			} else if (bili_logged_in()) {       /* 登录成功才切过去 */
				s_mode = want;
				s_page = 1;
				load_list();
			}
			s_hl_mode = -1;   /* 结束:高亮回落到真实频道 */
		}
	}

	/* 逐步落盘:万一还是卡在 "Closing software",trace.log 的最后一行
	 * 就指出卡在哪一步。异步日志在进程被杀时来不及写,所以用同步版。 */
	/* START 正常退出也先封网：缓存 worker 可能正在 playurl 短请求里，
	 * net_cancel_streams 掐不到它。先取消全部请求再 join，才能保证后面
	 * 拆 httpc/文件系统时不再有后台线程使用它们。 */
	net_shutdown_begin();
	ui_trace_sync("exit: cache worker");
	download_worker_stop();
	ui_trace_sync("exit: cache database");
	cache_manager_shutdown();
	ui_trace_sync("exit: thumb_exit");
	thumb_exit();
	ui_trace_sync("exit: net_exit");
	net_exit();
	/* 放在 net_exit 之后:tls 只在扫码登录时用,退出时它一定是空闲的,
	 * 不会像 httpc 那样有请求卡在里面 */
	ui_trace_sync("exit: tls_exit");
	tls_exit();
	ui_trace_sync("exit: ime_exit");
	ime_exit();
	ui_trace_sync("exit: ui_exit");
	ui_exit();
	return 0;
}
