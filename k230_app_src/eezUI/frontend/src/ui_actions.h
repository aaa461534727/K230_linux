/**
 * ui_actions.h — EEZ Studio 生成的 5 个 action 的具体实现 + 状态刷新
 *
 * EEZ 生成的 screens.c 只声明/调用 action_on_xxx_click() (见 ui/actions.h),
 * 具体"点了要干什么"在这里实现 —— 这样 EEZ 工程可以随时重新生成,
 * 我们的业务代码不会被覆盖。
 */
#ifndef K230UI_UI_ACTIONS_H
#define K230UI_UI_ACTIONS_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* lvgl 起来、ui_init() 之后调用一次: 起 500ms 状态轮询定时器 */
void app_init(const char *backend_sock);

/* 退出前收尾 (停后台线程) */
void app_deinit(void);

/* 由主循环调用 (每帧): 目前只做节流, 可留空 */
void app_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* K230UI_UI_ACTIONS_H */
