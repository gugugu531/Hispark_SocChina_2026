/* ui_lvgl — 板端触摸控制台 UI（LVGL v9 实现）。
 * 形态对齐 web-console:左侧视频 + 右侧参数侧边栏(状态 / 预设 / 热参数)。
 * 见 ui_lvgl.h 的"抽象口"说明。 */

#include "ui_lvgl.h"

#include "lvgl.h"

#include <stdio.h>

/* ================================================================
 * 调色板(对齐 web styles.css 的琥珀金暗色主题)
 * ================================================================ */
#define COL_BG        lv_color_hex(0x0c0a08)
#define COL_SURFACE   lv_color_hex(0x181512)
#define COL_ELEVATED  lv_color_hex(0x12100e)
#define COL_BORDER    lv_color_hex(0x2a251f)
#define COL_TEXT      lv_color_hex(0xe8e0d5)
#define COL_DIM       lv_color_hex(0x9e9688)
#define COL_GOLD      lv_color_hex(0xd4a017)
#define COL_AMBER     lv_color_hex(0xf59e0b)
#define COL_TEAL      lv_color_hex(0x14b8a6)
#define COL_ROSE      lv_color_hex(0xf43f5e)
#define COL_BLACK     lv_color_hex(0x000000)

#define SIDEBAR_W  388

/* ================================================================
 * 模块状态
 * ================================================================ */
static ui_cmd_cb g_cb;
static void     *g_user;
static int       g_updating;   /* 回填控件时置位,事件回调据此忽略,避免回环 */

/* 需要在 update 时刷新的控件 */
static lv_obj_t *g_badge;        /* 状态徽章 */
static lv_obj_t *g_lbl_fps, *g_lbl_infer, *g_lbl_txn, *g_lbl_drops,
                *g_lbl_frames, *g_lbl_mode, *g_lbl_nn, *g_lbl_out;
static lv_obj_t *g_sidebar;
static lv_obj_t *g_video;      /* 视频占位区(board overlay 模式下置透明,透出 VO 视频层) */
static lv_obj_t *g_video_ph;   /* 视频占位文字(overlay 模式隐藏,避免盖在视频上) */

static lv_obj_t *g_sw_enh, *g_sw_nn, *g_sw_tone, *g_sw_drc, *g_sw_ldci;
static lv_obj_t *g_sld_tone, *g_sld_guard, *g_sld_drc;
static lv_obj_t *g_lv_tone, *g_lv_guard, *g_lv_drc;  /* slider 数值标签 */

/* ================================================================
 * 小工具
 * ================================================================ */
static void send_cmd(const ui_cmd_t *c)
{
    if (g_cb && !g_updating) g_cb(c, g_user);
}

/* 通用卡片容器 */
static lv_obj_t *make_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (title) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, COL_GOLD, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    }
    return card;
}

/* 一行:左标签 + 右数值(返回数值标签,供 update 刷新) */
static lv_obj_t *make_kv(lv_obj_t *parent, const char *key)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, COL_DIM, 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, COL_TEXT, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    return v;
}

/* ================================================================
 * 事件回调
 * ================================================================ */
static void on_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    ui_cmd_t c = {0};
    if (sw == g_sw_enh)  { c.has_enhancement_enabled = 1; c.enhancement_enabled = on; }
    else if (sw == g_sw_nn)   { c.has_nn_clut_enabled = 1; c.nn_clut_enabled = on; }
    else if (sw == g_sw_tone) { c.has_tone_enabled = 1; c.tone_enabled = on; }
    else if (sw == g_sw_drc)  { c.has_drc_mode = 1; c.drc_mode = on; }
    else if (sw == g_sw_ldci) { c.has_ldci_mode = 1; c.ldci_mode = on; }
    send_cmd(&c);
}

static void on_slider(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(s);
    ui_cmd_t c = {0};
    if (s == g_sld_tone) {
        lv_label_set_text_fmt(g_lv_tone, "%.2f", v / 100.0f);
        c.has_tone_strength = 1; c.tone_strength = v / 100.0f;
    } else if (s == g_sld_guard) {
        lv_label_set_text_fmt(g_lv_guard, "%d", v);
        c.has_high_clip_guard = 1; c.high_clip_guard = (float)v;
    } else if (s == g_sld_drc) {
        lv_label_set_text_fmt(g_lv_drc, "%d", v);
        c.has_drc_strength = 1; c.drc_strength = v;
    }
    send_cmd(&c);
}

/* 预设:一次设定所有控件并下发一条组合命令 */
static void apply_preset(int enh, int nn, int tone, float tone_str,
                         int guard, int drc, int drc_str, int ldci)
{
    g_updating = 1;
    if (enh)  lv_obj_add_state(g_sw_enh, LV_STATE_CHECKED);  else lv_obj_remove_state(g_sw_enh, LV_STATE_CHECKED);
    if (nn)   lv_obj_add_state(g_sw_nn, LV_STATE_CHECKED);   else lv_obj_remove_state(g_sw_nn, LV_STATE_CHECKED);
    if (tone) lv_obj_add_state(g_sw_tone, LV_STATE_CHECKED); else lv_obj_remove_state(g_sw_tone, LV_STATE_CHECKED);
    if (drc)  lv_obj_add_state(g_sw_drc, LV_STATE_CHECKED);  else lv_obj_remove_state(g_sw_drc, LV_STATE_CHECKED);
    if (ldci) lv_obj_add_state(g_sw_ldci, LV_STATE_CHECKED); else lv_obj_remove_state(g_sw_ldci, LV_STATE_CHECKED);
    lv_slider_set_value(g_sld_tone, (int)(tone_str * 100.0f + 0.5f), LV_ANIM_OFF);
    lv_slider_set_value(g_sld_guard, guard, LV_ANIM_OFF);
    lv_slider_set_value(g_sld_drc, drc_str, LV_ANIM_OFF);
    lv_label_set_text_fmt(g_lv_tone, "%.2f", tone_str);
    lv_label_set_text_fmt(g_lv_guard, "%d", guard);
    lv_label_set_text_fmt(g_lv_drc, "%d", drc_str);
    g_updating = 0;

    ui_cmd_t c = {0};
    c.has_enhancement_enabled = 1; c.enhancement_enabled = enh;
    c.has_nn_clut_enabled = 1;     c.nn_clut_enabled = nn;
    c.has_tone_enabled = 1;        c.tone_enabled = tone;
    c.has_tone_strength = 1;       c.tone_strength = tone_str;
    c.has_high_clip_guard = 1;     c.high_clip_guard = (float)guard;
    c.has_drc_mode = 1;            c.drc_mode = drc;
    c.has_drc_strength = 1;        c.drc_strength = drc_str;
    c.has_ldci_mode = 1;           c.ldci_mode = ldci;
    send_cmd(&c);
}

static void on_preset(lv_event_t *e)
{
    long which = (long)lv_event_get_user_data(e);
    switch (which) {
    case 0: apply_preset(1, 1, 1, 0.70f, 10, 1, 512, 1); break; /* VIVID   */
    case 1: apply_preset(1, 1, 1, 0.30f,  5, 1, 512, 1); break; /* NATURAL */
    case 2: apply_preset(0, 0, 0, 0.00f,  3, 0,   0, 0); break; /* BYPASS  */
    default: break;
    }
}

static void on_toggle_sidebar(lv_event_t *e)
{
    (void)e;
    if (lv_obj_has_flag(g_sidebar, LV_OBJ_FLAG_HIDDEN))
        lv_obj_remove_flag(g_sidebar, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(g_sidebar, LV_OBJ_FLAG_HIDDEN);
}

/* ================================================================
 * 控件构造
 * ================================================================ */
static lv_obj_t *make_switch_row(lv_obj_t *parent, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 3, 0);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 28);
    lv_obj_set_style_bg_color(sw, COL_BORDER, 0);
    lv_obj_set_style_bg_color(sw, COL_GOLD, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* 标签 + 滑块 + 数值。out_val 接收数值标签指针。 */
static lv_obj_t *make_slider_row(lv_obj_t *parent, const char *label,
                                 int min, int max, lv_event_cb_t cb,
                                 lv_obj_t **out_val)
{
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, lv_pct(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(wrap, 4, 0);
    lv_obj_set_style_pad_row(wrap, 4, 0);

    lv_obj_t *hdr = lv_obj_create(wrap);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lv_label_create(hdr);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_t *v = lv_label_create(hdr);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, COL_AMBER, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    *out_val = v;

    lv_obj_t *s = lv_slider_create(wrap);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_set_height(s, 10);
    lv_slider_set_range(s, min, max);
    lv_obj_set_style_bg_color(s, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, COL_GOLD, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, COL_GOLD, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

static void make_preset_bar(lv_obj_t *parent)
{
    static const char *names[3] = {"VIVID", "NATURAL", "BYPASS"};
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);   /* 按钮间距,替代逐按钮 margin */
    for (long i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_set_flex_grow(b, 1);            /* 三等分,间距已由 pad_column 让出 */
        lv_obj_set_height(b, 44);
        lv_obj_set_style_bg_color(b, COL_ELEVATED, 0);
        lv_obj_set_style_border_color(b, COL_BORDER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 5, 0);
        lv_obj_set_style_pad_all(b, 0, 0);     /* 去默认按钮内边距 */
        lv_obj_set_style_shadow_width(b, 0, 0);/* 去默认主题阴影,防止画到边界外 */
        lv_obj_set_style_bg_color(b, COL_GOLD, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, on_preset, LV_EVENT_CLICKED, (void *)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, names[i]);
        lv_obj_center(l);
        lv_obj_set_style_text_color(l, COL_TEXT, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    }
}

/* ================================================================
 * 构建
 * ================================================================ */
void ui_lvgl_build(ui_cmd_cb cb, void *user)
{
    g_cb = cb;
    g_user = user;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 顶栏 ---- */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, 46);
    lv_obj_set_style_bg_color(header, COL_ELEVATED, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, COL_BORDER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 16, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "SOCCHINA");
    lv_obj_set_style_text_color(title, COL_GOLD, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    g_badge = lv_label_create(header);
    lv_label_set_text(g_badge, "--");
    lv_obj_set_style_margin_left(g_badge, 12, 0);
    lv_obj_set_style_pad_hor(g_badge, 10, 0);
    lv_obj_set_style_pad_ver(g_badge, 3, 0);
    lv_obj_set_style_radius(g_badge, 12, 0);
    lv_obj_set_style_bg_opa(g_badge, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(g_badge, COL_DIM, 0);
    lv_obj_set_style_text_color(g_badge, COL_DIM, 0);
    lv_obj_set_style_text_font(g_badge, &lv_font_montserrat_14, 0);

    /* 顶栏右侧:侧边栏折叠按钮 */
    lv_obj_t *spacer = lv_obj_create(header);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);

    lv_obj_t *gear = lv_button_create(header);
    lv_obj_set_size(gear, 40, 34);
    lv_obj_set_style_bg_color(gear, COL_BORDER, 0);
    lv_obj_add_event_cb(gear, on_toggle_sidebar, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(gear);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gl);
    lv_obj_set_style_text_color(gl, COL_TEXT, 0);

    /* ---- 主体:左视频 + 右侧边栏 ---- */
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* 视频占位(板端:此区透明,透出 VO 视频层;模拟器:画占位) */
    lv_obj_t *video = lv_obj_create(body);
    g_video = video;
    lv_obj_set_flex_grow(video, 1);
    lv_obj_set_height(video, lv_pct(100));
    lv_obj_set_style_bg_color(video, COL_BLACK, 0);
    lv_obj_set_style_border_width(video, 0, 0);
    lv_obj_set_style_radius(video, 0, 0);
    lv_obj_remove_flag(video, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *vph = lv_label_create(video);
    g_video_ph = vph;
    lv_label_set_text(vph, LV_SYMBOL_VIDEO "  LIVE 1024x576\n(camera passthrough)");
    lv_obj_set_style_text_align(vph, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(vph, lv_color_hex(0x6e6250), 0);
    lv_obj_set_style_text_font(vph, &lv_font_montserrat_20, 0);
    lv_obj_center(vph);

    /* 右侧参数侧边栏 */
    g_sidebar = lv_obj_create(body);
    lv_obj_set_width(g_sidebar, SIDEBAR_W);
    lv_obj_set_height(g_sidebar, lv_pct(100));
    lv_obj_set_style_bg_color(g_sidebar, COL_BG, 0);
    lv_obj_set_style_border_color(g_sidebar, COL_BORDER, 0);
    lv_obj_set_style_border_width(g_sidebar, 0, 0);
    lv_obj_set_style_border_side(g_sidebar, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(g_sidebar, 1, 0);
    lv_obj_set_style_radius(g_sidebar, 0, 0);
    lv_obj_set_style_pad_all(g_sidebar, 12, 0);
    lv_obj_set_style_pad_row(g_sidebar, 12, 0);
    lv_obj_set_flex_flow(g_sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_sidebar, LV_DIR_VER);

    /* 状态卡 */
    lv_obj_t *stc = make_card(g_sidebar, "STATUS");
    g_lbl_mode   = make_kv(stc, "Capture / FPS");
    g_lbl_fps    = make_kv(stc, "Display FPS");
    g_lbl_infer  = make_kv(stc, "Infer p95");
    g_lbl_txn    = make_kv(stc, "LUT txn p95");
    g_lbl_drops  = make_kv(stc, "Drops / Timeout");
    g_lbl_nn     = make_kv(stc, "NN degraded");
    g_lbl_out    = make_kv(stc, "HDMI / RTSP / Viewers");
    g_lbl_frames = make_kv(stc, "Frames / LUT");

    /* 预设卡 */
    lv_obj_t *pc = make_card(g_sidebar, "PRESETS");
    make_preset_bar(pc);

    /* 热参数卡 */
    lv_obj_t *cc = make_card(g_sidebar, "REALTIME CONTROL");
    g_sw_enh  = make_switch_row(cc, "Enhancement", on_switch);
    g_sw_nn   = make_switch_row(cc, "AI Color (NN CLUT)", on_switch);
    g_sw_tone = make_switch_row(cc, "Tone Mapping", on_switch);
    g_sld_tone  = make_slider_row(cc, "Tone Strength", 0, 100, on_slider, &g_lv_tone);
    g_sld_guard = make_slider_row(cc, "Clip Guard", 0, 100, on_slider, &g_lv_guard);
    g_sw_drc  = make_switch_row(cc, "DRC Auto", on_switch);
    g_sld_drc   = make_slider_row(cc, "DRC Strength", 0, 1023, on_slider, &g_lv_drc);
    g_sw_ldci = make_switch_row(cc, "LDCI Auto", on_switch);
}

/* ================================================================
 * 刷新
 * ================================================================ */
static void set_badge(const char *state)
{
    lv_color_t c = COL_DIM;
    if (state && state[0] == 'R') c = COL_TEAL;        /* RUNNING */
    else if (state && state[0] == 'D') c = COL_AMBER;  /* DEGRADED */
    else if (state && state[0] == 'F') c = COL_ROSE;   /* FAILED */
    lv_label_set_text(g_badge, state ? state : "--");
    lv_obj_set_style_text_color(g_badge, c, 0);
    lv_obj_set_style_bg_color(g_badge, c, 0);
}

/* 板端 overlay 模式：屏幕与视频占位区置透明（alpha=0），合成时视频透出、UI chrome 覆盖。
 * 顶栏/侧边栏/卡片仍不透明。sim 不调用此函数 → 保持不透明预览。 */
void ui_lvgl_set_overlay_mode(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    if (g_video) {
        lv_obj_set_style_bg_opa(g_video, LV_OPA_TRANSP, 0);
    }
    /* 视频区占位文字在合成路径下会盖住相机画面,overlay 模式隐藏之(sim 不调用本函数,保留预览)。 */
    if (g_video_ph) {
        lv_obj_add_flag(g_video_ph, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_lvgl_update(const ui_state_t *st)
{
    if (!st || !g_badge) return;

    set_badge(st->state_str);
    lv_label_set_text_fmt(g_lbl_mode, "%s / %d",
                          st->capture_mode ? "wdr2to1" : "linear", st->target_fps);
    lv_label_set_text_fmt(g_lbl_fps, "%.1f", st->fps);
    lv_label_set_text_fmt(g_lbl_infer, "%.2f ms", st->infer_p95);
    lv_label_set_text_fmt(g_lbl_txn, "%.2f ms", st->txn_p95);
    lv_label_set_text_fmt(g_lbl_drops, "%ld / %ld", st->stream_drops, st->timeouts);
    lv_label_set_text(g_lbl_nn, st->nn_degraded ? "YES" : "no");
    lv_obj_set_style_text_color(g_lbl_nn, st->nn_degraded ? COL_AMBER : COL_TEAL, 0);
    lv_label_set_text_fmt(g_lbl_out, "%d / %d / %d",
                          st->hdmi, st->rtsp, st->viewers);
    lv_label_set_text_fmt(g_lbl_frames, "%llu / %llu",
                          st->stream_frames, st->lut_updates);

    /* 回填控件(屏蔽事件,不反向下发) */
    g_updating = 1;
    if (st->enhancement_enabled) lv_obj_add_state(g_sw_enh, LV_STATE_CHECKED); else lv_obj_remove_state(g_sw_enh, LV_STATE_CHECKED);
    if (st->nn_enabled)          lv_obj_add_state(g_sw_nn, LV_STATE_CHECKED);  else lv_obj_remove_state(g_sw_nn, LV_STATE_CHECKED);
    if (st->tone_enabled)        lv_obj_add_state(g_sw_tone, LV_STATE_CHECKED);else lv_obj_remove_state(g_sw_tone, LV_STATE_CHECKED);
    if (st->drc_mode)            lv_obj_add_state(g_sw_drc, LV_STATE_CHECKED); else lv_obj_remove_state(g_sw_drc, LV_STATE_CHECKED);
    if (st->ldci_mode)           lv_obj_add_state(g_sw_ldci, LV_STATE_CHECKED);else lv_obj_remove_state(g_sw_ldci, LV_STATE_CHECKED);

    /* 仅当用户未按住该滑块时回填,避免拖动被打断 */
    if (!lv_obj_has_state(g_sld_tone, LV_STATE_PRESSED)) {
        lv_slider_set_value(g_sld_tone, (int)(st->tone_strength * 100.0f + 0.5f), LV_ANIM_OFF);
        lv_label_set_text_fmt(g_lv_tone, "%.2f", st->tone_strength);
    }
    if (!lv_obj_has_state(g_sld_guard, LV_STATE_PRESSED)) {
        lv_slider_set_value(g_sld_guard, (int)(st->high_clip_guard + 0.5f), LV_ANIM_OFF);
        lv_label_set_text_fmt(g_lv_guard, "%d", (int)(st->high_clip_guard + 0.5f));
    }
    if (!lv_obj_has_state(g_sld_drc, LV_STATE_PRESSED)) {
        lv_slider_set_value(g_sld_drc, st->drc_strength, LV_ANIM_OFF);
        lv_label_set_text_fmt(g_lv_drc, "%d", st->drc_strength);
    }
    g_updating = 0;
}
