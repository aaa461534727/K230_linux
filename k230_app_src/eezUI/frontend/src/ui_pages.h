/**
 * ui_pages.h — 4 个服务的二级画布 (中文说明 + 重新连接 + 返回)
 *
 * 为什么单独一个文件:
 *   `ui/` 里的 screens.c 是 EEZ Studio 生成的 (会覆盖), 不能在里面加页面;
 *   这里用纯 LVGL API 手写二级页面, 和生成的主界面共存:
 *     主界面 (EEZ) --点按钮--> pages_show() + be_select() --> 该服务的画布
 *                            (画布一直在, 服务在后台等投屏, 不刷黑)
 *     画布「重新连接」--> be_select() 再发一次 (后端幂等)
 *     画布「返回主界面」--> be_stop_session() + pages_show_main()
 *
 * 页面记忆 (2026-11-07):
 *   投屏真正出帧时后端会停掉 k230ui 把屏让给 drmplay, 视频流断开后再把 k230ui
 *   拉起来。为了让重启后的 k230ui 直接回到「刚才点的那个按钮」的画布,
 *   pages_show() 把当前 service 写进 /run/k230-ui/active-page,
 *   pages_restore() 在 app_init 时读回来并自动 pages_show();
 *   pages_show_main() / 后端的 ui 模式负责清掉这个文件。
 *
 * 中文显示:
 *   用 `font_cjk.h` 里 lv_font_conv 生成的子集字体 (20/28px);
 *   改了本文件里的中文文案, 要重跑 `tools/make-cjk-font.py` 补字。
 */
#ifndef K230UI_PAGES_H
#define K230UI_PAGES_H

#include "backend_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 页面下标 (顺序和主界面按钮一致) */
enum {
    PAGE_MIRACAST = 0,
    PAGE_AIRPLAY,
    PAGE_CARLIFE,
    PAGE_CARPLAY,
    PAGE_COUNT
};

/* 把主界面 (EEZ 生成的英文界面) 换成中文文案 + 中文字体 */
void pages_apply_main_texts(void);

/* 进入某个服务的画布 (只换画布 + 记忆页面, 不启动投屏; 启动由调用者 be_select) */
void pages_show(int index);

/* 回主界面 (调用者先 be_stop_session); 同时清掉页面记忆文件 */
void pages_show_main(void);

/* 进程刚被后端拉起时调用: 读 /run/k230-ui/active-page 自动回到上次那个画布 */
void pages_restore(void);

/* 当前在哪个画布: -1 = 主界面 */
int pages_active(void);

/* 状态轮询用: 把后端状态刷到当前画布的状态行 */
void pages_update_status(const be_status_t *st);

/* 模拟器自测用: 直接触发某画布的「重新连接」按钮 (没有就返回 0) */
int pages_sim_click_start(int index);

#ifdef __cplusplus
}
#endif

#endif /* K230UI_PAGES_H */
