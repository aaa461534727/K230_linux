/**
 * main_board.c —— 板端 (K230, Linux + DRM) 入口
 *
 * 和 main_sdl.c 的区别只在"画到哪儿 / 触摸从哪来":
 *   本机 : SDL2 窗口 + 鼠标
 *   板端 : SDK 的 liblvgl_linux.so 提供的 k230_driver_init() —— 内部走
 *          lv_linux_drm (DRM/KMS + 双缓冲 + K230 旋转补丁) + evdev (触摸屏)
 * 渲染/业务代码完全共用: ui 目录里 EEZ 生成的 .c + src/ui_actions.c + backend_client.c
 *
 * ⚠️ k230_driver_init(NULL, 0) 的语义 (SDK 源码 src/lib/display_backends/drm_k230_v4l2.c):
 *      d == NULL      -> 用 "DRM" 后端 (标准 lv_linux_drm), 不涉及摄像头 v4l2 层
 *      flag == 0      -> 不起 v4l2-drm 线程
 *    传 struct display* 的那条路是"LVGL 与摄像头同屏"用的, 我们不需要。
 *    lv_init() 也在它内部调, 所以外面不要再调一次。
 *
 * 板端前提 (k230-ui.service 已经写好):
 *   · k230-screen.service (drmplay) 必须先停 —— DRM master 只能有一个;
 *   · /usr/lib/liblvgl*.so 来自 SDK buildroot (LVGL 9.6.0), 不用自带。
 */
#include "lvgl/lvgl.h"
#include "ui.h"
#include "ui_actions.h"
#include "screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* liblvgl_linux.so 导出, 但 SDK 没把 driver_backends.h 装进 sysroot ⇒ 自己声明。
 * 原型见 SDK: src/lib/driver_backends.h:80 */
extern int k230_driver_init(void *display, char v4l2_drm_run_flag);

/* 板端 SDK 的 lv_conf.h 开了 LV_USE_PERF_MONITOR (=1), 屏幕右下角会一直挂着
 * "30 FPS | CPU (0% | 0%)" 的调试角标 —— 正式界面不能带这个。
 * 开关在 liblvgl.so 里编译死了, 但 sysmon 有运行时隐藏接口; 用 weak 引用,
 * 模拟器那份 LVGL 没开 SYSMON 时自动跳过 (不会链接失败)。 */
extern void lv_sysmon_hide_performance(lv_display_t *disp) __attribute__((weak));

int main(void)
{
    uint32_t idle;

    printf("[k230ui] init DRM display + evdev touch\n");
    if (k230_driver_init(NULL, 0) < 0) {
        fprintf(stderr, "[k230ui] k230_driver_init failed "
                        "(DRM 被别的进程占着? 检查 k230-screen.service)\n");
        return 1;
    }

    ui_init();                        /* EEZ: 建屏 + 载入 main */

    /* 去掉 SDK 自带的性能角标 (右下角 "30 FPS | CPU ..."), 正式界面不要调试信息 */
    if (lv_sysmon_hide_performance) {
        lv_sysmon_hide_performance(lv_display_get_default());
    }

    app_init(getenv("K230UI_SOCK"));  /* 后端客户端 + 500ms 状态轮询 */

    printf("[k230ui] main loop\n");
    for (;;) {
        idle = lv_timer_handler();
        if (idle == 0) idle = 1;
        if (idle > 10) idle = 10;
        usleep(idle * 1000);
    }

    /* 不会到这里; 由 systemd 发 SIGTERM 结束 */
    return 0;
}
