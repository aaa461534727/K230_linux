/**
 * ui_pages.h — 4 个服务的二级画布 (中文说明 + 开始投屏 + 返回)
 *
 * 为什么单独一个文件:
 *   `ui/` 里的 screens.c 是 EEZ Studio 生成的 (会覆盖), 不能在里面加页面;
 *   这里用纯 LVGL API 手写二级页面, 和生成的主界面共存:
 *     主界面 (EEZ) --点按钮--> pages_show() --> 该服务的画布
 *     画布「返回」--> pages_show_main() --> 主界面
 *     画布「开始投屏」--> be_select() --> 后端让屏给投屏 (画面顶替画布)
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

/* 进入某个服务的画布 (不启动投屏, 等用户点「开始投屏」) */
void pages_show(int index);

/* 回主界面 (调用前由调用者决定是否 be_stop_session) */
void pages_show_main(void);

/* 当前在哪个画布: -1 = 主界面 */
int pages_active(void);

/* 状态轮询用: 把后端状态刷到当前画布的状态行 */
void pages_update_status(const be_status_t *st);

/* 模拟器自测用: 直接触发某画布的「开始投屏」按钮 (没有就返回 0) */
int pages_sim_click_start(int index);

#ifdef __cplusplus
}
#endif

#endif /* K230UI_PAGES_H */
