/**
 * ui_pages.c — 4 个服务二级画布的实现 (中文说明 + 重新连接 + 返回)
 *
 * 交互 (2026-11-07 改版):
 *   主界面点 Miracast  -> pages_show(PAGE_MIRACAST) + be_select("miracast")
 *                         (服务立刻起, 但屏仍归本画布, 不刷黑)
 *   真正出帧           -> 后端停 k230ui, 屏交给 drmplay 刷视频流
 *   视频流断开         -> 后端重新拉起 k230ui, pages_restore() 自动回到本画布
 *   画布「返回主界面」 -> be_stop_session() + pages_show_main() (清页面记忆)
 *
 * 为什么不再有「开始投屏」:
 *   主人要求点主界面按钮就直接自动起服务、一直停在这个界面; 后端在 waiting 态
 *   只起服务不让屏, 所以画布能一直显示, 不需要第二次点击。原按钮改成「重新连接」
 *   (后端对同一路重复 SELECT 幂等), 只在服务异常时手动补一刀用。
 *
 * 中文: 字体来自 font_cjk_20 / font_cjk_28 (lv_font_conv 子集, 见 tools/make-cjk-font.py)。
 */
#include "ui_pages.h"
#include "backend_client.h"
#include "font_cjk.h"
#include "screens.h"
#include "ui.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 页面记忆文件: 后端从 video 回到 waiting 会重启 k230ui, 靠它回到刚才那个按钮 */
#define ACTIVE_PAGE_FILE "/run/k230-ui/active-page"

extern objects_t objects;

#define COL_BG      0x0b1220
#define COL_FG      0xe6edf3
#define COL_BODY    0xcfd8e3
#define COL_MUTED   0x8b98a9
#define COL_OK      0x34d399
#define COL_WARN    0xfbbf24
#define COL_ERR     0xf87171
#define COL_BTN2    0x1b2537
#define COL_LINE    0x2c3a52

typedef struct {
    const char *service;    /* 后端动作名: miracast / airplay / carlife / carplay */
    const char *btn_name;   /* 主界面按钮上的英文名 (画布小标题用不到, 留作日志) */
    const char *title;      /* 画布标题 (中文) */
    const char *subtitle;   /* 画布副标题 */
    const char *body;       /* 使用说明 (多行) */
    uint32_t    accent;     /* 主色 (和主界面按钮同色) */

    lv_obj_t   *scr;
    lv_obj_t   *lbl_status;
    lv_obj_t   *btn_start;
    int         started;    /* 是否已经发过 SELECT (返回时要 STOP) */
} page_t;

static page_t s_pages[PAGE_COUNT] = {
    {
        "miracast", "Miracast",
        "Miracast 无线投屏",
        "安卓手机 / Windows 电脑",
        "1. 在手机上打开「无线投屏」或「多屏互动」\n"
        "2. 在设备列表中选择「K230-CAST」\n"
        "3. 首次连接时，在手机上点「允许」\n"
        "4. 连接成功后，自动显示手机画面\n"
        "\n"
        "提示：手机不需要连接本机的 Wi-Fi。",
        0x2f6fed, NULL, NULL, NULL, 0,
    },
    {
        "airplay", "AirPlay",
        "AirPlay 镜像投屏",
        "iPhone / iPad / Mac",
        "1. 手机连接 Wi-Fi「K230-CARPLAY」\n"
        "   密码：k230carplay\n"
        "2. 打开控制中心，点「屏幕镜像」\n"
        "3. 在列表中选择「K230-CAST」\n"
        "4. 连接成功后，自动显示手机画面\n"
        "5. 反控：在手机「设置 → 蓝牙」里\n"
        "   连接「K230-CAST」\n"
        "\n"
        "提示：画面走 Wi-Fi，反控走蓝牙。\n"
        "两个都连上，才能触摸操作手机。",
        0x1f9d55, NULL, NULL, NULL, 0,
    },
    {
        "carlife", "CarLife",
        "百度 CarLife+",
        "安卓手机车机互联",
        "1. 在手机上安装「CarLife+」App\n"
        "2. 连接 Wi-Fi「K230-CARPLAY」\n"
        "   密码：k230carplay\n"
        "3. 打开 CarLife+，选择无线连接\n"
        "4. 连接成功后，自动显示车机画面\n"
        "\n"
        "提示：无线方式不需要数据线。",
        0xd97706, NULL, NULL, NULL, 0,
    },
    {
        "carplay", "CarPlay",
        "Apple CarPlay",
        "iPhone 车载互联",
        "1. 手机连接 Wi-Fi「K230-CARPLAY」\n"
        "   密码：k230carplay\n"
        "2. 打开 设置 → 通用 → CarPlay\n"
        "3. 选择「K230」完成配对\n"
        "4. 连接成功后，自动显示车机画面\n"
        "\n"
        "提示：无线 CarPlay 仍在调试中。",
        0x7c3aed, NULL, NULL, NULL, 0,
    },
};

static int s_active = -1;

/* ------------------------------------------------------------------ 小工具 */
static void set_label_text(lv_obj_t *lbl, const char *text)
{
    if (!lbl) return;
    const char *old = lv_label_get_text(lbl);
    if (old && !strcmp(old, text)) return;
    lv_label_set_text(lbl, text);
}

static void set_status(page_t *p, const char *text, uint32_t color)
{
    if (!p->lbl_status) return;
    set_label_text(p->lbl_status, text);
    lv_obj_set_style_text_color(p->lbl_status, lv_color_hex(color), 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                            int w, int h, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_size(lbl, w, h);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, text);
    return lbl;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y,
                             int w, int h, uint32_t bg, int bordered)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, 255, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, bordered ? 1 : 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COL_LINE), 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &font_cjk_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_FG), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

/* ------------------------------------------------------------------ 回调 */
/* 「重新连接」: 再发一次 SELECT。后端在 waiting/running 态对同一路幂等,
 * 只有服务意外挂了才有意义 (此时后端会重新 spawn wait 模式把服务拉回来)。 */
static void on_retry_clicked(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);

    if (!p) return;
    p->started = 1;
    set_status(p, "正在重新连接投屏服务，请稍候…", COL_WARN);
    be_select(p->service);
}

static void on_back_clicked(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);

    (void)p;
    /* 自动起服务后画布背后一直有服务在跑, 返回主界面必须收掉它 */
    be_stop_session();
    pages_show_main();
}

/* ------------------------------------------------------------------ 建页面 */
static void page_create(page_t *p)
{
    lv_obj_t *scr;
    lv_obj_t *bar;

    if (p->scr) return;

    scr = lv_obj_create(NULL);
    p->scr = scr;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, 255, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* 标题 + 副标题 */
    make_label(scr, p->title, 24, 26, 432, 44, &font_cjk_28, COL_FG);
    make_label(scr, p->subtitle, 24, 74, 432, 30, &font_cjk_20, COL_MUTED);

    /* 主色横条 */
    bar = lv_obj_create(scr);
    lv_obj_set_pos(bar, 24, 112);
    lv_obj_set_size(bar, 432, 4);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, lv_color_hex(p->accent), 0);
    lv_obj_set_style_bg_opa(bar, 255, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    /* 使用说明 */
    {
        lv_obj_t *body = make_label(scr, p->body, 24, 136, 432, 380,
                                    &font_cjk_20, COL_BODY);
        lv_obj_set_style_text_line_space(body, 8, 0);
    }

    /* 状态行 */
    p->lbl_status = make_label(scr, "正在等待设备连接…", 24, 536, 432, 66,
                               &font_cjk_20, COL_MUTED);
    lv_obj_set_style_text_line_space(p->lbl_status, 4, 0);

    /* 重新连接 (服务已在后台自动起, 这个按钮只是异常时的手动补刀) */
    p->btn_start = make_button(scr, "重新连接", 24, 614, 432, 82, p->accent, 0);
    lv_obj_add_event_cb(p->btn_start, on_retry_clicked, LV_EVENT_CLICKED, p);

    /* 返回 (不点就一直停在这个画布, 包括等待投屏和投屏结束之后) */
    {
        lv_obj_t *btn_back = make_button(scr, "返回主界面", 24, 710, 432, 66,
                                         COL_BTN2, 1);
        lv_obj_add_event_cb(btn_back, on_back_clicked, LV_EVENT_CLICKED, p);
    }
}

/* ------------------------------------------------------------------ 对外 */
void pages_apply_main_texts(void)
{
    /* 标题/副标题/状态行换成中文 (字体也要换, 否则中文是方框) */
    if (objects.lbl_title) {
        lv_obj_set_style_text_font(objects.lbl_title, &font_cjk_28, 0);
        lv_label_set_text(objects.lbl_title, "K230 无线投屏");
    }
    if (objects.lbl_subtitle) {
        lv_obj_set_style_text_font(objects.lbl_subtitle, &font_cjk_20, 0);
        lv_label_set_text(objects.lbl_subtitle, "点击下方按钮，选择投屏方式");
    }
    if (objects.lbl_status) {
        lv_obj_set_style_text_font(objects.lbl_status, &font_cjk_20, 0);
        lv_label_set_long_mode(objects.lbl_status, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(objects.lbl_status, 440, 80);
        lv_label_set_text(objects.lbl_status, "准备就绪");
    }

    /* 按钮文案: 英文名 + 中文功能 (两行, 20px 中文) */
    {
        struct { lv_obj_t *lbl; const char *text; } btns[] = {
            { objects.btn_miracast_label, "Miracast\n无线投屏" },
            { objects.btn_airplay_label,  "AirPlay\n镜像投屏" },
            { objects.btn_carlife_label,  "CarLife\n车机互联" },
            { objects.btn_carplay_label,  "CarPlay\n车载互联" },
        };
        size_t i;

        for (i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
            if (!btns[i].lbl) continue;
            lv_obj_set_style_text_font(btns[i].lbl, &font_cjk_20, 0);
            lv_obj_set_style_text_align(btns[i].lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(btns[i].lbl, 0, 45);
            lv_obj_set_size(btns[i].lbl, 210, 60);
            lv_label_set_text(btns[i].lbl, btns[i].text);
        }
    }
}

/* 把当前画布记到 /run, 后端重启 k230ui 后靠它回到同一个按钮的二级页 */
static void remember_page(const char *service)
{
    FILE *f = fopen(ACTIVE_PAGE_FILE, "w");
    if (!f) return;
    fputs(service, f);
    fclose(f);
}

/* 服务名 -> 页面下标 (和 s_pages[].service 对应) */
static int page_index_of(const char *service)
{
    int i;
    if (!service || !*service || !strcmp(service, "none")) return -1;
    for (i = 0; i < PAGE_COUNT; i++)
        if (!strcmp(s_pages[i].service, service)) return i;
    return -1;
}

void pages_show(int index)
{
    page_t *p;

    if (index < 0 || index >= PAGE_COUNT) return;
    p = &s_pages[index];
    page_create(p);

    p->started = 0;
    set_status(p, "正在等待设备连接，请在手机上操作…", COL_WARN);
    remember_page(p->service);

    s_active = index;
    fprintf(stderr, "[k230ui] pages_show: %s (index=%d)\n", p->service, index);
    lv_scr_load_anim(p->scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

void pages_show_main(void)
{
    unlink(ACTIVE_PAGE_FILE);       /* 回主界面 = 忘掉刚才那个按钮 */
    s_active = -1;
    fprintf(stderr, "[k230ui] pages_show_main\n");
    loadScreen(SCREEN_ID_MAIN);
}

void pages_restore(void)
{
    char buf[32] = { 0 };
    FILE *f = fopen(ACTIVE_PAGE_FILE, "r");
    int idx;

    if (!f) {                       /* 首次开机 / 主界面态: 没有记忆, 留在主界面 */
        fprintf(stderr, "[k230ui] pages_restore: none (主界面)\n");
        return;
    }
    if (fgets(buf, sizeof(buf), f)) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
    }
    fclose(f);

    idx = page_index_of(buf);
    if (idx < 0) {                  /* 内容不认识: 不去猜, 留主界面 */
        fprintf(stderr, "[k230ui] pages_restore: 未知页面 '%s', 留主界面\n", buf);
        return;
    }
    fprintf(stderr, "[k230ui] pages_restore: 回到上次按钮的二级页 %s\n", buf);
    pages_show(idx);               /* 只显示画布, 不再发 SELECT (后端已经在 waiting) */
}

int pages_active(void)
{
    return s_active;
}

void pages_update_status(const be_status_t *st)
{
    page_t *p;
    char buf[192];

    if (s_active < 0 || !st) return;
    p = &s_pages[s_active];

    if (!st->connected) {
        set_status(p, "后端未连接（k230-ui-backend）", COL_ERR);
        return;
    }

    /* 后端在别的路上: 提示一下, 但画布不动 (除非用户点返回主界面) */
    if ((st->state == BE_STATE_WAITING || st->state == BE_STATE_RUNNING ||
         st->state == BE_STATE_STARTING) &&
        strcmp(st->service, p->service) != 0) {
        snprintf(buf, sizeof(buf), "后端正在处理 %s，可返回主界面重新选择", st->service);
        set_status(p, buf, COL_WARN);
        return;
    }

    switch (st->state) {
    case BE_STATE_STARTING:
        snprintf(buf, sizeof(buf), "正在启动 %s 投屏服务…", p->btn_name);
        set_status(p, buf, COL_WARN);
        break;
    case BE_STATE_WAITING:
        snprintf(buf, sizeof(buf), "%s 服务已就绪，正在等待设备连接…", p->btn_name);
        set_status(p, buf, COL_WARN);
        break;
    case BE_STATE_RUNNING:
        set_status(p, "已连接，正在投屏…", COL_OK);
        break;
    case BE_STATE_STOPPING:
        set_status(p, "正在停止…", COL_WARN);
        break;
    case BE_STATE_ERROR:
        snprintf(buf, sizeof(buf), "启动失败：%s",
                 st->message[0] ? st->message : "未知错误");
        set_status(p, buf, COL_ERR);
        break;
    case BE_STATE_IDLE:
        set_status(p, "服务已停止，可点「重新连接」或返回主界面", COL_MUTED);
        break;
    default:
        set_status(p, "后端状态未知", COL_WARN);
        break;
    }
}

int pages_sim_click_start(int index)
{
    if (index < 0 || index >= PAGE_COUNT) return 0;
    if (!s_pages[index].btn_start) return 0;
    lv_obj_send_event(s_pages[index].btn_start, LV_EVENT_CLICKED, NULL);
    return 1;
}
