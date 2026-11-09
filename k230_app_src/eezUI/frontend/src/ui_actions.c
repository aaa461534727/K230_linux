/**
 * ui_actions.c — 主界面 4 个按钮 + 状态标签的业务逻辑
 *
 * 交互约定 (2026-11-05 起加二级画布, 见 src/ui_pages.c):
 *   - 点某个服务按钮       -> 进入该服务的画布 (中文说明 + 开始投屏 + 返回)
 *   - 画布「开始投屏」     -> 才真正启动那一路 (后端负责停掉其它路、让出屏幕)
 *   - 画布「返回」         -> 停掉正在启动/运行的那一路, 回主界面
 *   - 底部标签实时显示后端状态; 后端没起来就显示 "后端未连接"
 *
 * 为什么点按钮不直接启动: 后端的 select 会立刻让屏给投屏 (UI 被停掉),
 *   直接启动的话用户来不及看画布上的连接说明。
 */
#include "ui_actions.h"
#include "backend_client.h"
#include "actions.h"
#include "screens.h"
#include "ui.h"
#include "ui_pages.h"

#include <stdio.h>
#include <string.h>

/* EEZ 生成的对象表 (screens.c 里定义) */
extern objects_t objects;

/* 颜色 (和 EEZ 工程里的取值保持一致, 改一处颜色要两边一起改) */
#define COL_MUTED   0x8b98a9
#define COL_OK      0x34d399
#define COL_WARN    0xfbbf24
#define COL_ERR     0xf87171
#define COL_FG      0xe6edf3

static lv_obj_t *s_active_btn = NULL;   /* 当前高亮的按钮 */
static lv_timer_t *s_poll_timer = NULL;

/* ------------------------------------------------------------------ 小工具 */
static const char *service_label(const char *id)
{
    if (!id) return "None";
    if (!strcmp(id, "miracast")) return "Miracast";
    if (!strcmp(id, "airplay"))  return "AirPlay";
    if (!strcmp(id, "carlife"))  return "CarLife";
    if (!strcmp(id, "carplay"))  return "CarPlay";
    return "None";
}

static lv_obj_t *button_for_service(const char *id)
{
    if (!id) return NULL;
    if (!strcmp(id, "miracast")) return objects.btn_miracast;
    if (!strcmp(id, "airplay"))  return objects.btn_airplay;
    if (!strcmp(id, "carlife"))  return objects.btn_carlife;
    if (!strcmp(id, "carplay"))  return objects.btn_carplay;
    return NULL;
}

static void set_highlight(lv_obj_t *btn)
{
    if (s_active_btn == btn)
        return;

    if (s_active_btn) {
        lv_obj_set_style_border_width(s_active_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    s_active_btn = btn;
    if (s_active_btn) {
        lv_obj_set_style_border_width(s_active_btn, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_active_btn, lv_color_hex(0xffffff),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void set_status_text(const char *text, uint32_t color)
{
    if (!objects.lbl_status) return;
    lv_label_set_text(objects.lbl_status, text);
    lv_obj_set_style_text_color(objects.lbl_status, lv_color_hex(color),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* 点按钮的统一处理已移到二级画布 (src/ui_pages.c):
 * 主界面只负责跳转, 真正启动/停止在画布上的「开始投屏」/「返回」里。 */

/* 点主界面按钮: 进该服务的画布 (启动动作在画布里的「开始投屏」) */
void action_on_miracast_click(lv_event_t *e) { (void)e; pages_show(PAGE_MIRACAST); }
void action_on_airplay_click(lv_event_t *e)  { (void)e; pages_show(PAGE_AIRPLAY); }
void action_on_carlife_click(lv_event_t *e)  { (void)e; pages_show(PAGE_CARLIFE); }
void action_on_carplay_click(lv_event_t *e)  { (void)e; pages_show(PAGE_CARPLAY); }

void action_on_main_loaded(lv_event_t *e)
{
    (void)e;
    /* 主界面文案换成中文 (EEZ 生成的是英文, 字体也是 Montserrat) */
    pages_apply_main_texts();
    set_status_text("准备就绪，请选择投屏方式", COL_MUTED);
}

/* --------------------------------------------------------- 状态轮询 (500ms) */
static void poll_status_cb(lv_timer_t *t)
{
    be_status_t st;
    char buf[192];

    (void)t;
    be_get_status(&st);

    /* 二级画布自己刷状态行 (不在画布上时是空操作) */
    pages_update_status(&st);

    switch (st.state) {
    case BE_STATE_IDLE:
        set_highlight(NULL);
        set_status_text("准备就绪，请选择投屏方式", COL_MUTED);
        break;

    case BE_STATE_STARTING:
        set_highlight(button_for_service(st.service));
        snprintf(buf, sizeof(buf), "正在启动 %s…", service_label(st.service));
        set_status_text(buf, COL_WARN);
        break;

    case BE_STATE_RUNNING:
        set_highlight(button_for_service(st.service));
        snprintf(buf, sizeof(buf), "%s 投屏中",
                 service_label(st.service));
        set_status_text(buf, COL_OK);
        break;

    case BE_STATE_STOPPING:
        set_status_text("正在停止…", COL_WARN);
        break;

    case BE_STATE_ERROR:
        set_highlight(NULL);
        snprintf(buf, sizeof(buf), "出错：%s",
                 st.message[0] ? st.message : "未知错误");
        set_status_text(buf, COL_ERR);
        break;

    default:
        set_highlight(NULL);
        set_status_text(st.connected ? "后端状态未知" : "后端未连接（k230-ui-backend）",
                        st.connected ? COL_WARN : COL_ERR);
        break;
    }
}

/* ------------------------------------------------------------------ 生命周期 */
void app_init(const char *backend_sock)
{
    be_start(backend_sock);
    if (!s_poll_timer)
        s_poll_timer = lv_timer_create(poll_status_cb, 500, NULL);
    poll_status_cb(NULL);
}

void app_deinit(void)
{
    if (s_poll_timer) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    be_shutdown();
}

void app_tick(void)
{
    /* 目前不需要每帧动作; 留接口给以后做动画/长按 */
}
