/**
 * font_cjk.h — 中文字体 (lv_font_conv 生成的子集字体)
 *
 * 为什么不用 LVGL 内置字体:
 *   板端 liblvgl.so 只带了 16px 的 CJK 子集, 标题太小、字也不一定全;
 *   这里按项目里实际用到的汉字打子集, 20px/28px 两档, 保证一个都不缺。
 *
 * 谁生成的:
 *   `python3 tools/make-cjk-font.py` (自动扫 src/ 下 .c 里的非 ASCII 字符,
 *   调用 lv_font_conv + 本机 DroidSansFallbackFull.ttf 生成 font_cjk_20.c / font_cjk_28.c)
 *   ⚠ 改了界面文案 (ui_pages.c / ui_actions.c 里的中文) 要重新跑一次本脚本,
 *     否则新字会显示成方框。
 */
#ifndef K230UI_FONT_CJK_H
#define K230UI_FONT_CJK_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t font_cjk_20;   /* 正文 / 按钮 / 状态行 */
extern const lv_font_t font_cjk_28;   /* 页面标题 / 主界面标题 */

#ifdef __cplusplus
}
#endif

#endif /* K230UI_FONT_CJK_H */
