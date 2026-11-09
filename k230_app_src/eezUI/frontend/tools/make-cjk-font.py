#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make-cjk-font.py — 按界面里实际出现的中文, 生成 LVGL 子集字体.

为什么要有这个脚本:
  LVGL 不能直接渲染 TTF (板端没有中文字库, freetype 又要额外带字体文件),
  标准做法是用 lv_font_conv 把用到的字打成 C 数组. 手写字表容易漏,
  所以这里直接扫 `src/*.c` 的所有非 ASCII 字符 —— 界面里出现什么字就带什么字.

用法:
  cd K230/src/eezUI/frontend
  python3 tools/make-cjk-font.py

输出 (生成物, 不要手改):
  src/font_cjk_20.c   (正文/按钮, 20px)
  src/font_cjk_28.c   (标题, 28px)

依赖: lv_font_conv (npm i -g lv_font_conv) + 一款中文字体 (默认 DroidSansFallbackFull.ttf)
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FRONTEND = os.path.dirname(HERE)
SRC_DIR = os.path.join(FRONTEND, "src")

FONT = os.environ.get(
    "K230UI_CJK_TTF",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
)
# 拉丁字母/数字单独从 DejaVu 取 (DroidSansFallback 是"中文兜底字体", 不含 ASCII)
FONT_LATIN = os.environ.get(
    "K230UI_LATIN_TTF",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
)

# 档位: (字号, 输出文件, C 里的变量名)
SIZES = [
    (20, "font_cjk_20.c", "font_cjk_20"),
    (28, "font_cjk_28.c", "font_cjk_28"),
]

# 除 ASCII 之外固定要带的符号 (界面里用到的中文标点/箭头)
EXTRA = "「」『』【】，。、：；！？…“”‘’（）《》—·→"


def collect_chars():
    """只取 C 字符串字面量里的非 ASCII 字 (注释里的中文不带, 免得字库白白变大)."""
    import re

    chars = set(EXTRA)
    # 匹配 "..." (允许 \" 转义); 注释里的引号内容极少, 不影响正确性
    literal = re.compile(r'"((?:[^"\\]|\\.)*)"')

    # 只扫会出现在界面上的两个文件 (生成物 font_cjk_*.c 自己不要扫, 否则字表滚雪球)
    for name in ("ui_pages.c", "ui_actions.c"):
        path = os.path.join(SRC_DIR, name)
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        for m in literal.finditer(text):
            for ch in m.group(1):
                if ord(ch) > 0x7E:
                    chars.add(ch)
    return "".join(sorted(chars))


def main():
    if not os.path.isfile(FONT):
        sys.exit("找不到中文字体: %s (可用 K230UI_CJK_TTF 指定)" % FONT)
    if not os.path.isfile(FONT_LATIN):
        sys.exit("找不到拉丁字体: %s (可用 K230UI_LATIN_TTF 指定)" % FONT_LATIN)

    symbols = collect_chars()
    print("子集字符数: %d" % len(symbols))

    for size, out_name, lv_name in SIZES:
        out = os.path.join(SRC_DIR, out_name)
        cmd = [
            "lv_font_conv",
            "--bpp", "4",
            "--size", str(size),
            # ⚠ 顺序有讲究 (2026-11-05 实测): 中文兜底字体必须放**前**, DejaVu 放后。
            #   反过来的话 lv_font_conv 会把 U+2192 (→) 当成"前者已声明但无字形"而丢掉,
            #   结果界面上 "设置 → 通用" 里的箭头变成空白格。
            # 1) 中文: DroidSansFallback, 只带界面里出现过的字
            "--font", FONT,
            "--symbols", symbols,
            # 2) 拉丁/数字 + 箭头: DejaVu (DroidSansFallback 不含 ASCII, 也没有 →)
            "--font", FONT_LATIN,
            "-r", "0x20-0x7E",
            "-r", "0x2190-0x2193",
            # 板端 lv_conf.h 里 LV_USE_FONT_COMPRESSED=0 ⇒ 压缩字库会 "Couldn't get the bitmap"
            "--no-compress",
            "--no-prefilter",
            "--format", "lvgl",
            "--lv-include", "lvgl/lvgl.h",
            "--lv-font-name", lv_name,
            "-o", out,
        ]
        print("生成 %s (%dpx)" % (out_name, size))
        subprocess.run(cmd, check=True)
        print("  -> %s (%d 字节)" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
