/**
 * lv_conf.h — 本机模拟器 (SDL2) 用的 LVGL 配置
 *
 * 板端不用这个文件: 板上是 SDK buildroot 编好的 liblvgl.so (LVGL 9.6.0),
 * 它的配置在 SDK 自己的 lv_conf.h 里 (DRM + evdev + K230 旋转补丁)。
 *
 * 与板端对齐的关键项:
 *   - 32bpp 色深 (板端 LV_COLOR_DEPTH=32)
 *   - 逻辑分辨率 480x800 (由 main_sdl.c 建窗口时给, 不是编译期常量)
 *   - 同一套 Montserrat 字体 16/20/24/28 (EEZ 生成的代码按名字引用)
 * LVGL 9.6 起 LV_COLOR_DEPTH 被 LV_COLOR_FORMAT_DEFAULT 取代, 这里用新名字。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   颜色 / 布局
 *===================*/
/* 32bpp, 与板端一致 (XRGB8888) */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888
#define LV_DPI_DEF 130
#define LV_DEF_REFR_PERIOD 16

/*====================
   系统 / 内存
 *===================*/
/* 单线程主循环: SDL 后端在 lv_timer_handler() 里跑, 不需要 OS 抽象 */
#define LV_USE_OS LV_OS_NONE

#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (1024U * 1024U)

/*====================
   日志
 *===================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/*====================
   字体 (EEZ 生成的 screens.c 直接用这几个)
 *===================*/
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

/*====================
   显示 / 输入后端
 *===================*/
/* 本机: SDL2 窗口 + 鼠标当触摸 */
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#define LV_SDL_BACKEND LV_SDL_BACKEND_SW
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_ACCELERATED 0
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 1

/* 板端才用, 本机模拟关掉 */
#define LV_USE_LINUX_DRM 0
#define LV_USE_LINUX_FBDEV 0

#endif /*LV_CONF_H*/
