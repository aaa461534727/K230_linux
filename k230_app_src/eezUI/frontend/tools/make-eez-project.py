#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 K230 主界面的 EEZ Studio 工程文件 (k230-ui.eez-project).

为什么用脚本生成而不是手改 JSON:
  .eez-project 是 4 MB 级的 JSON, 手改容易漏字段; 用脚本生成保证每次结构一致,
  改布局/文案只改本文件的 LAYOUT 区, 再重新生成即可。
  生成后请用 EEZ Studio 打开 `k230-ui.eez-project` 做视觉微调 ——
  之后以 EEZ Studio 保存的文件为准, 不要再跑本脚本覆盖 (会丢手工改动)。

用法:
  cd K230/src/eezUI/frontend
  python3 tools/make-eez-project.py

输出:
  eez-project/k230-ui.eez-project
"""

import json
import os
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(ROOT, "eez-project")
OUT_FILE = os.path.join(OUT_DIR, "k230-ui.eez-project")

# ---------------------------------------------------------------- 布局参数
DISPLAY_W = 480
DISPLAY_H = 800

BG = "#0b1220"
FG = "#e6edf3"
MUTED = "#8b98a9"

BTN_W = 210
BTN_H = 150
COL_X = (20, 250)
ROW_Y = (200, 400)
BTN_RADIUS = 18

BUTTONS = [
    # (identifier, 文案, 颜色, 列, 行, 后端动作名)
    ("btn_miracast", "Miracast", "#2f6fed", 0, 0, "on_miracast_click"),
    ("btn_airplay", "AirPlay", "#1f9d55", 1, 0, "on_airplay_click"),
    ("btn_carlife", "CarLife", "#d97706", 0, 1, "on_carlife_click"),
    ("btn_carplay", "CarPlay", "#7c3aed", 1, 1, "on_carplay_click"),
]

TEXT_FONT = "MONTSERRAT_24"
TITLE_FONT = "MONTSERRAT_28"
SUB_FONT = "MONTSERRAT_16"
STATUS_FONT = "MONTSERRAT_20"

WIDGET_FLAGS_BTN = (
    "CLICK_FOCUSABLE|GESTURE_BUBBLE|PRESS_LOCK|SCROLL_CHAIN_HOR|SCROLL_CHAIN_VER|"
    "SCROLL_ELASTIC|SCROLL_MOMENTUM|SCROLL_ON_FOCUS|SCROLL_WITH_ARROW|SNAPPABLE|EVENT_BUBBLE"
)
WIDGET_FLAGS_LBL = (
    "CLICK_FOCUSABLE|GESTURE_BUBBLE|PRESS_LOCK|SCROLLABLE|SCROLL_CHAIN_HOR|"
    "SCROLL_CHAIN_VER|SCROLL_ELASTIC|SCROLL_MOMENTUM|SCROLL_WITH_ARROW|SNAPPABLE"
)
WIDGET_FLAGS_SCR = (
    "CLICKABLE|PRESS_LOCK|CLICK_FOCUSABLE|GESTURE_BUBBLE|SNAPPABLE|SCROLLABLE|"
    "SCROLL_ELASTIC|SCROLL_MOMENTUM|SCROLL_CHAIN_HOR|SCROLL_CHAIN_VER"
)


def uid():
    return str(uuid.uuid4())


def style_ref():
    return {
        "objID": uid(),
        "useStyle": "default",
        "conditionalStyles": [],
        "childStyles": [],
    }


def local_styles(definition):
    return {"objID": uid(), "definition": definition}


def label(identifier, text, left, top, font, color, width=None, height=None,
          text_align="LEFT"):
    return {
        "objID": uid(),
        "type": "LVGLLabelWidget",
        "left": left,
        "top": top,
        "width": width if width is not None else 120,
        "height": height if height is not None else 30,
        "customInputs": [],
        "customOutputs": [],
        "style": style_ref(),
        "timeline": [],
        "eventHandlers": [],
        "identifier": identifier,
        "leftUnit": "px",
        "topUnit": "px",
        "widthUnit": "content" if width is None else "px",
        "heightUnit": "content" if height is None else "px",
        "children": [],
        "widgetFlags": WIDGET_FLAGS_LBL,
        "hiddenFlagType": "literal",
        "clickableFlagType": "literal",
        "flagScrollbarMode": "",
        "flagScrollDirection": "",
        "scrollSnapX": "",
        "scrollSnapY": "",
        "checkedStateType": "literal",
        "disabledStateType": "literal",
        "states": "",
        "localStyles": local_styles(
            {
                "MAIN": {
                    "DEFAULT": {
                        "text_font": font,
                        "text_color": color,
                        "text_align": text_align,
                    }
                }
            }
        ),
        "group": "",
        "groupIndex": 0,
        "preprocess": False,
        "text": text,
        "textType": "literal",
        "longMode": "WRAP",
        "recolor": False,
    }


def button(identifier, text, action, x, y, color):
    return {
        "objID": uid(),
        "type": "LVGLButtonWidget",
        "left": x,
        "top": y,
        "width": BTN_W,
        "height": BTN_H,
        "customInputs": [],
        "customOutputs": [],
        "style": style_ref(),
        "timeline": [],
        "eventHandlers": [
            {
                "objID": uid(),
                "eventName": "CLICKED",
                "handlerType": "action",
                "action": action,
                "keyValue": "ANY",
                "userData": 0,
                "chfocus": "",
                "focused": False,
                "userDataById": False,
                "userDataExtra": 0,
            }
        ],
        "identifier": identifier,
        "leftUnit": "px",
        "topUnit": "px",
        "widthUnit": "px",
        "heightUnit": "px",
        "children": [
            label(
                identifier + "_label",
                text,
                0,
                (BTN_H - 32) // 2,
                TEXT_FONT,
                "#ffffff",
                width=BTN_W,
                height=32,
                text_align="CENTER",
            )
        ],
        "widgetFlags": WIDGET_FLAGS_BTN,
        "hiddenFlagType": "literal",
        "clickableFlag": True,
        "clickableFlagType": "literal",
        "flagScrollbarMode": "",
        "flagScrollDirection": "",
        "scrollSnapX": "",
        "scrollSnapY": "",
        "checkedStateType": "literal",
        "disabledStateType": "literal",
        "states": "",
        "localStyles": local_styles(
            {
                "MAIN": {
                    "DEFAULT": {
                        "bg_color": color,
                        "bg_opa": 255,
                        "radius": BTN_RADIUS,
                        "border_width": 0,
                        "shadow_width": 0,
                        "pad_left": 0,
                        "pad_right": 0,
                        "pad_top": 0,
                        "pad_bottom": 0,
                    },
                    "PRESSED": {"bg_color": "#1b2537", "transform_zoom": 243},
                    "FOCUSED": {"border_width": 2, "border_color": "#ffffff"},
                }
            }
        ),
        "group": "",
        "groupIndex": 0,
    }


def make_screen():
    children = [
        label("lbl_title", "K230 Cast", 20, 40, TITLE_FONT, FG),
        label("lbl_subtitle", "Tap a button to start that service", 20, 96, SUB_FONT, MUTED),
        label("lbl_status", "Status: ready", 20, 700, STATUS_FONT, MUTED),
    ]
    for ident, text, color, col, row, action in BUTTONS:
        children.append(button(ident, text, action, COL_X[col], ROW_Y[row], color))

    return {
        "objID": uid(),
        "components": [
            {
                "objID": uid(),
                "type": "LVGLScreenWidget",
                "left": 0,
                "top": 0,
                "width": DISPLAY_W,
                "height": DISPLAY_H,
                "customInputs": [],
                "customOutputs": [],
                "style": style_ref(),
                "timeline": [],
                "eventHandlers": [
                    {
                        "objID": uid(),
                        "eventName": "SCREEN_LOAD_START",
                        "handlerType": "action",
                        "action": "on_main_loaded",
                        "keyValue": "ANY",
                        "userData": 0,
                        "chfocus": "",
                        "focused": False,
                        "userDataById": False,
                        "userDataExtra": 0,
                    }
                ],
                "leftUnit": "px",
                "topUnit": "px",
                "widthUnit": "px",
                "heightUnit": "px",
                "children": children,
                "widgetFlags": WIDGET_FLAGS_SCR,
                "hiddenFlagType": "literal",
                "clickableFlag": True,
                "clickableFlagType": "literal",
                "checkedStateType": "literal",
                "disabledStateType": "literal",
                "states": "",
                "localStyles": local_styles({"MAIN": {"DEFAULT": {"bg_color": BG}}}),
                "groupIndex": 0,
            }
        ],
        "connectionLines": [],
        "localVariables": [],
        "userProperties": [],
        "name": "main",
        "left": 0,
        "top": 0,
        "width": DISPLAY_W,
        "height": DISPLAY_H,
        "isUsedAsUserWidget": False,
        "isTopLayer": False,
        "isPageDisabled": False,
    }


# ---------------------------------------------------- 代码生成模板 (照搬 EEZ 官方)
BUILD_FILES = [
    ("screens.h", "#ifndef EEZ_LVGL_UI_SCREENS_H\r\n#define EEZ_LVGL_UI_SCREENS_H\r\n\r\n//${eez-studio LVGL_INCLUDE}\r\n\r\n#ifdef __cplusplus\r\nextern \"C\" {\r\n#endif\r\n\r\n//${eez-studio LVGL_SCREENS_DECL}\r\n//${eez-studio LVGL_SCREENS_DECL_EXT}\r\n\r\n#ifdef __cplusplus\r\n}\r\n#endif\r\n\r\n#endif /*EEZ_LVGL_UI_SCREENS_H*/"),
    ("screens.c", "#include <string.h>\n\n#include \"screens.h\"\n#include \"images.h\"\n#include \"fonts.h\"\n#include \"actions.h\"\n#include \"vars.h\"\n#include \"styles.h\"\n#include \"ui.h\"\n\n//${eez-studio LVGL_SCREENS_DEF}\n//${eez-studio LVGL_SCREENS_DEF_EXT}"),
    ("actions.h", "#ifndef EEZ_LVGL_UI_EVENTS_H\r\n#define EEZ_LVGL_UI_EVENTS_H\r\n\r\n//${eez-studio LVGL_INCLUDE}\r\n\r\n#ifdef __cplusplus\r\nextern \"C\" {\r\n#endif\r\n\r\n//${eez-studio LVGL_ACTIONS_DECL}\r\n\r\n#ifdef __cplusplus\r\n}\r\n#endif\r\n\r\n#endif /*EEZ_LVGL_UI_EVENTS_H*/"),
    ("vars.h", "#ifndef EEZ_LVGL_UI_VARS_H\r\n#define EEZ_LVGL_UI_VARS_H\r\n\r\n#include <stdint.h>\r\n#include <stdbool.h>\r\n\r\n#ifdef __cplusplus\r\nextern \"C\" {\r\n#endif\r\n\r\n// enum declarations\r\n\r\n//${eez-studio FLOW_ENUMS}\r\n\r\n// Flow global variables\r\n\r\n//${eez-studio FLOW_GLOBAL_VARIABLES_ENUM}\r\n\r\n// Native global variables\r\n\r\n//${eez-studio LVGL_VARS_DECL}\r\n\r\n#ifdef __cplusplus\r\n}\r\n#endif\r\n\r\n#endif /*EEZ_LVGL_UI_VARS_H*/"),
    ("structs.h", "#ifndef EEZ_LVGL_UI_STRUCTS_H\n#define EEZ_LVGL_UI_STRUCTS_H\n\n//${eez-studio EEZ_FOR_LVGL_CHECK}\n\n#if defined(EEZ_FOR_LVGL)\n\n#include <eez/flow/flow.h>\n#include <stdint.h>\n#include <stdbool.h>\n\n#include \"vars.h\"\n\nusing namespace eez;\n\n//${eez-studio FLOW_STRUCTS}\n\n//${eez-studio FLOW_STRUCT_VALUES}\n\n#endif\n\n#endif /*EEZ_LVGL_UI_STRUCTS_H*/\n"),
    ("images.h", "#ifndef EEZ_LVGL_UI_IMAGES_H\n#define EEZ_LVGL_UI_IMAGES_H\n\n//${eez-studio LVGL_INCLUDE}\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n//${eez-studio LVGL_IMAGES_DECL}\n\n#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_IMAGES_H*/\n"),
    ("images.c", "#include \"images.h\"\n\n//${eez-studio LVGL_IMAGES_DEF}"),
    ("fonts.h", "#ifndef EEZ_LVGL_UI_FONTS_H\r\n#define EEZ_LVGL_UI_FONTS_H\r\n\r\n//${eez-studio LVGL_INCLUDE}\r\n\r\n#ifdef __cplusplus\r\nextern \"C\" {\r\n#endif\r\n\r\n//${eez-studio LVGL_FONTS_DECL}\r\n\r\n#ifdef __cplusplus\r\n}\r\n#endif\r\n\r\n#endif /*EEZ_LVGL_UI_FONTS_H*/"),
    ("styles.h", "#ifndef EEZ_LVGL_UI_STYLES_H\r\n#define EEZ_LVGL_UI_STYLES_H\r\n\r\n//${eez-studio LVGL_INCLUDE}\r\n\r\n#ifdef __cplusplus\r\nextern \"C\" {\r\n#endif\r\n\r\n//${eez-studio LVGL_STYLES_DECL}\r\n\r\n#ifdef __cplusplus\r\n}\r\n#endif\r\n\r\n#endif /*EEZ_LVGL_UI_STYLES_H*/"),
    ("styles.c", "#include \"styles.h\"\n\n//${eez-studio LVGL_STYLES_DEF}"),
    ("ui.h", "#ifndef EEZ_LVGL_UI_GUI_H\n#define EEZ_LVGL_UI_GUI_H\n\n//${eez-studio LVGL_INCLUDE}\n\n//${eez-studio EEZ_FOR_LVGL_CHECK}\n\n#if defined(EEZ_FOR_LVGL)\n#include <eez/flow/lvgl_api.h>\n#endif\n\n#if !defined(EEZ_FOR_LVGL)\n#include \"screens.h\"\n#endif\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n//${eez-studio GUI_ASSETS_DECL}\n\nvoid ui_init();\nvoid ui_tick();\n\n#if !defined(EEZ_FOR_LVGL)\nvoid loadScreen(enum ScreensEnum screenId);\n#endif\n\n#ifdef __cplusplus\n}\n#endif\n\n#endif // EEZ_LVGL_UI_GUI_H\n"),
    ("ui.c", "#include <string.h>\n\n#if defined(EEZ_FOR_LVGL)\n#include <eez/core/vars.h>\n#endif\n\n#include \"ui.h\"\n#include \"screens.h\"\n#include \"images.h\"\n#include \"actions.h\"\n#include \"vars.h\"\n\n//${eez-studio GUI_ASSETS_DEF}\n\n//${eez-studio LVGL_NATIVE_VARS_TABLE_DEF}\n\n//${eez-studio LVGL_ACTIONS_ARRAY_DEF}\n\n#if defined(EEZ_FOR_LVGL)\n\nvoid ui_init() {\n    eez_flow_init(assets, sizeof(assets), (lv_obj_t **)&objects, sizeof(objects), images, sizeof(images), actions);\n}\n\nvoid ui_tick() {\n    eez_flow_tick();\n    tick_screen(g_currentScreen);\n}\n\n#else\n\nstatic int16_t currentScreen = -1;\n\nstatic lv_obj_t *getLvglObjectFromIndex(int32_t index) {\n    if (index == -1) {\n        return 0;\n    }\n    return ((lv_obj_t **)&objects)[index];\n}\n\nstatic const void *getLvglImageByName(const char *name) {\n    for (size_t imageIndex = 0; imageIndex < sizeof(images) / sizeof(ext_img_desc_t); imageIndex++) {\n        if (strcmp(images[imageIndex].name, name) == 0) {\n            return images[imageIndex].img_dsc;\n        }\n    }\n    return 0;\n}\n\nvoid loadScreen(enum ScreensEnum screenId) {\n    currentScreen = screenId - 1;\n    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);\n    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);\n}\n\nvoid ui_init() {\n    create_screens();\n    loadScreen(SCREEN_ID_MAIN);\n}\n\nvoid ui_tick() {\n    tick_screen(currentScreen);\n}\n\n#endif\n"),
]


def action(name):
    return {
        "objID": uid(),
        "components": [],
        "connectionLines": [],
        "localVariables": [],
        "userProperties": [],
        "name": name,
        "implementationType": "native",
    }


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    project = {
        "themesVersion": 1,
        "objID": uid(),
        "settings": {
            "objID": uid(),
            "general": {
                "objID": uid(),
                "projectVersion": "v3",
                "projectType": "lvgl",
                "lvglVersion": "9.5.0",
                "extensions": [],
                "imports": [],
                "flowSupport": False,
                "displayWidth": DISPLAY_W,
                "displayHeight": DISPLAY_H,
                "displayBorderRadius": 0,
                "colorFormat": "RGB",
                "resourceFiles": [],
                "hiddenWidgetLines": "visible",
                "dimmedLinesOpacity": "20",
            },
            "build": {
                "objID": uid(),
                "configurations": [{"objID": uid(), "name": "Default"}],
                "files": [
                    {"objID": uid(), "fileName": n, "template": t} for n, t in BUILD_FILES
                ],
                # 相对 eez-project/ 目录: 生成到 ../ui (= frontend/ui)
                "destinationFolder": "../ui",
                "separateFolderForImagesAndFonts": False,
                "lvglInclude": "lvgl/lvgl.h",
                "customLvglIncludePath": "",
                "generateSourceCodeForEezFramework": False,
                "compressFlowDefinition": False,
                "executionQueueSize": 3000,
                "expressionEvaluatorStackSize": 100,
                "generateGenerallyLibforVSCode": False,
                "VSCodedestinationFolder": ".",
                "VSCodetoolChain": "",
                "VSCodecflags": [{"objID": uid(), "cflags": "-g -O0"}],
                "generateGenerallyLibforRtos": False,
                "RtostoolChain": "",
                "Rtoscflags": [{"objID": uid(), "cflags": "-g -O0"}],
                "generateGenerallyLibforLinux": False,
                "LinuxtoolChain": "",
                "Linuxcflags": [{"objID": uid(), "cflags": "-g -O0"}],
                "deleteSource": False,
            },
        },
        "variables": {"objID": uid(), "globalVariables": []},
        "actions": [action(a) for _, _, _, _, _, a in BUTTONS]
        + [action("on_main_loaded")],
        "userPages": [make_screen()],
        "userWidgets": [],
        "lvglStyles": {"objID": uid(), "styles": [], "defaultStyles": {}},
        "lvglGroups": {"objID": uid(), "groups": []},
        "fonts": [],
        "texts": {
            "objID": uid(),
            "languages": [{"objID": uid(), "languageID": "EN"}],
            "resources": [],
        },
        "bitmaps": [],
        "colors": [],
        "themes": [{"objID": uid(), "name": "Default", "colors": []}],
    }

    with open(OUT_FILE, "w", encoding="utf-8") as f:
        json.dump(project, f, ensure_ascii=False, indent=1)

    print("written:", OUT_FILE)
    print("pages:", [p["name"] for p in project["userPages"]])
    print("actions:", [a["name"] for a in project["actions"]])


if __name__ == "__main__":
    main()
