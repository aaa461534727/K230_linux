/**
 * main_sdl.c — 本机模拟器入口 (PC 上用 SDL2 开一个 480x800 的窗口跑同一套 UI)
 *
 * 干什么: 在 Ubuntu 上不接板子就能看/点主界面, 用鼠标当触摸。
 *         渲染代码和板端是同一份 (ui 目录下 EEZ 生成的 .c + src/ui_actions.c), 只有
 *         "画到哪儿 / 触摸从哪来" 不同 (本机 SDL, 板端 DRM+evdev)。
 * 怎么用: bash scripts/build-sim.sh && bash scripts/run-sim.sh
 * 环境变量:
 *   K230UI_SOCK         后端 socket 路径 (默认 /run/k230-ui/backend.sock)
 *   K230UI_SIM_CLICK    启动 1.5s 后模拟点某个按钮 (miracast/airplay/carlife/carplay),
 *                       只用于自动截图验证 (走真 LVGL 事件, 顺便验证 EEZ 绑定)
 *   K230UI_SIM_EXIT_MS  跑够多少毫秒自动退出 (截图/CI 用; 不设就一直开着)
 */
#include "lvgl/lvgl.h"
#include "ui_actions.h"
#include "ui_pages.h"
#include "ui.h"
#include "screens.h"   /* EEZ 生成: extern objects_t objects; 各控件指针 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIM_W 480
#define SIM_H 800

static unsigned long g_start_ms;

/* 1.5s 后模拟点击, 只为自动截图/自测; 正常用鼠标点 */
static void maybe_auto_click(void)
{
    const char *svc = getenv("K230UI_SIM_CLICK");
    lv_obj_t *btn = NULL;

    if (!svc || !*svc)
        return;

    if (!strcmp(svc, "miracast"))      btn = objects.btn_miracast;
    else if (!strcmp(svc, "airplay"))  btn = objects.btn_airplay;
    else if (!strcmp(svc, "carlife"))  btn = objects.btn_carlife;
    else if (!strcmp(svc, "carplay"))  btn = objects.btn_carplay;

    if (btn) {
        fprintf(stderr, "[sim] auto-click %s\n", svc);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    }
}

int main(int argc, char **argv)
{
    const char *env;
    unsigned long exit_ms = 0;
    int clicked = 0;
    int started = 0;
    int want_start;
    lv_display_t *disp;

    (void)argc;
    (void)argv;

    lv_init();

    /* 480x800 = 板端竖屏分辨率; 1:1 显示 (屏幕小的话可用 set_zoom 缩小) */
    disp = lv_sdl_window_create(SIM_W, SIM_H);
    if (!disp) {
        fprintf(stderr, "[sim] failed to create SDL window (DISPLAY 没设?)\n");
        return 1;
    }
    lv_sdl_window_set_title(disp, "K230 Cast UI (sim)");

    /* 鼠标当触摸; 键盘/滚轮顺手加上, 方便调试 */
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
    lv_sdl_mousewheel_create();

    ui_init();                       /* EEZ 生成: 建屏 + 载入 main */
    app_init(getenv("K230UI_SOCK")); /* 起后端客户端 + 500ms 状态轮询 */

    if ((env = getenv("K230UI_SIM_EXIT_MS")) && *env)
        exit_ms = (unsigned long)strtoul(env, NULL, 10);

    /* 自动点「开始投屏」: 只为截图验证画布上的按钮/状态行 */
    want_start = getenv("K230UI_SIM_START") != NULL;

    g_start_ms = (unsigned long)lv_tick_get();

    for (;;) {
        uint32_t idle = lv_timer_handler();
        if (idle > 10) idle = 10;   /* 最多睡 10ms, 保证点击响应 */
        usleep(idle * 1000);

        if (!clicked && (unsigned long)lv_tick_get() - g_start_ms > 1500) {
            clicked = 1;
            maybe_auto_click();
        }

        if (want_start && !started && clicked &&
            (unsigned long)lv_tick_get() - g_start_ms > 3000) {
            int idx = pages_active();
            started = 1;
            fprintf(stderr, "[sim] auto-click start on page %d\n", idx);
            if (idx >= 0) pages_sim_click_start(idx);
        }

        if (exit_ms && (unsigned long)lv_tick_get() - g_start_ms > exit_ms)
            break;
    }

    app_deinit();
    return 0;
}
