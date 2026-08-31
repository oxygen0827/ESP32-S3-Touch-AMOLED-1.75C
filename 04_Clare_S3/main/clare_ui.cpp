#include "clare_ui.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl_bsp.h"

// Full GB2312 CJK font (7540 glyphs, Arial Unicode source, lv_font_conv).
// The built-in lv_font_source_han_sans_sc_16_cjk only covers LVGL's demo
// charset, which is why live Chinese transcripts rendered as tofu "口".
LV_FONT_DECLARE(lv_font_clare_cjk_16);

/*
 * Round-screen UI (466x466 CO5300 AMOLED, physical circle radius 233).
 *
 * Design rules for the round glass (do not regress to the C6 square layout):
 *  - Anything important must sit inside the inscribed square (330x330 centered,
 *    i.e. x/y in [68, 398]) or on the vertical center axis where the chord is
 *    widest. Rectangular header bars and corner-anchored grids get clipped.
 *  - Status text rides the top center chord (short, single line).
 *  - Primary actions are circles/pills clustered around the bottom center;
 *    a circle's lowest point is at its center-x, so round buttons can sit
 *    lower than a rectangle of the same width.
 *  - Scrollable content lives in a centered 330-wide card whose corners were
 *    verified to fall inside the glass (corner (68,140): r=189 < 233).
 * Reference: XiaoZhi board package for this panel pads its status bar 10% from
 * each side and centers all primary content the same way.
 */

namespace {

enum class Action : uint8_t {
    OpenClare,
    CloseClare,
    StartMeeting,
    StopMeeting,
    ToggleHost,
    Refresh,
    OpenDemo,
};

constexpr int kScreenSize = 466;
constexpr int kContentWidth = 330;   // inscribed-square width of the round glass

clare_ui_callbacks_t s_callbacks = {};
lv_obj_t *s_screen = nullptr;
lv_obj_t *s_home = nullptr;
lv_obj_t *s_clare = nullptr;
lv_obj_t *s_demo = nullptr;
lv_obj_t *s_wifi = nullptr;
lv_obj_t *s_clare_wifi = nullptr;
lv_obj_t *s_status = nullptr;
lv_obj_t *s_transcript = nullptr;
lv_obj_t *s_answer = nullptr;
lv_obj_t *s_start_btn = nullptr;
lv_obj_t *s_stop_btn = nullptr;
lv_obj_t *s_host_btn = nullptr;
lv_obj_t *s_start_label = nullptr;
lv_obj_t *s_stop_label = nullptr;
lv_obj_t *s_host_label = nullptr;
char s_transcript_text[1536] = {};
char s_transcript_partial[256] = {};
char s_answer_text[2048] = {};
char s_answer_partial[384] = {};
bool s_ui_initialized = false;
bool s_meeting_active = false;
bool s_host_active = false;

static void set_page_locked(clare_ui_page_t page);

static void refresh_transcript_locked(void)
{
    if (!s_transcript) return;
    lv_label_set_text_fmt(s_transcript, "%s%s%s", s_transcript_text,
                          s_transcript_text[0] && s_transcript_partial[0] ? "\n" : "",
                          s_transcript_partial);
    lv_obj_scroll_to_view(s_transcript, LV_ANIM_OFF);
}

static void refresh_answer_locked(void)
{
    if (!s_answer) return;
    lv_label_set_text_fmt(s_answer, "%s%s%s", s_answer_text,
                          s_answer_text[0] && s_answer_partial[0] ? "\n" : "",
                          s_answer_partial);
    lv_obj_scroll_to_view(s_answer, LV_ANIM_OFF);
}

static void append_bounded(char *dst, size_t dst_len, const char *text)
{
    if (!dst || dst_len == 0 || !text || !text[0]) return;
    size_t used = strlen(dst);
    if (used >= dst_len - 1) return;
    size_t copy = strlen(text);
    if (copy > dst_len - used - 1) copy = dst_len - used - 1;
    memcpy(dst + used, text, copy);
    dst[used + copy] = '\0';
}

static lv_color_t color(uint32_t value) { return lv_color_hex(value); }

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t fg,
                            lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color(fg), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static void dispatch_action(Action action)
{
    switch (action) {
    case Action::OpenClare:
        set_page_locked(CLARE_UI_CLARE);
        if (s_callbacks.open_clare) s_callbacks.open_clare(s_callbacks.ctx);
        break;
    case Action::CloseClare:
        set_page_locked(CLARE_UI_HOME);
        if (s_callbacks.close_clare) s_callbacks.close_clare(s_callbacks.ctx);
        break;
    case Action::StartMeeting:
        if (s_callbacks.start_meeting) s_callbacks.start_meeting(s_callbacks.ctx);
        break;
    case Action::StopMeeting:
        if (s_callbacks.stop_meeting) s_callbacks.stop_meeting(s_callbacks.ctx);
        break;
    case Action::ToggleHost:
        if (s_callbacks.toggle_host) s_callbacks.toggle_host(s_callbacks.ctx);
        break;
    case Action::Refresh:
        if (s_callbacks.refresh_summary) s_callbacks.refresh_summary(s_callbacks.ctx);
        break;
    case Action::OpenDemo:
        set_page_locked(CLARE_UI_DEMO);
        break;
    }
}

static void add_action_cb(lv_obj_t *obj, Action action)
{
    lv_obj_add_event_cb(obj, [](lv_event_t *event) {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
        dispatch_action(static_cast<Action>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
    }, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
}

/* Round or pill button sized for thumb taps on the round glass. */
static lv_obj_t *make_touch_button(lv_obj_t *parent, const char *title, Action action,
                                   int width, int height, bool circle,
                                   uint32_t bg, uint32_t fg = 0xF4F7FB,
                                   const lv_font_t *font = &lv_font_montserrat_16)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_min_width(button, width, 0);
    lv_obj_set_style_radius(button, circle ? LV_RADIUS_CIRCLE : height / 2, 0);
    lv_obj_set_style_bg_color(button, color(bg), 0);
    lv_obj_set_style_bg_color(button, color(0x334867), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, color(0x2A3242), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    add_action_cb(button, action);
    lv_obj_t *label = make_label(button, title, fg, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(label, font, 0);
    return button;
}

/* Small circular back button anchored on the top-left chord (verified visible). */
static void make_back_button(lv_obj_t *page)
{
    lv_obj_t *back = make_touch_button(page, LV_SYMBOL_LEFT, Action::CloseClare,
                                       44, 44, true, 0x1D2939, 0xF4F7FB,
                                       &lv_font_montserrat_16);
    lv_obj_set_pos(back, 62, 50);
}

static lv_obj_t *make_page(uint32_t bg)
{
    lv_obj_t *page = lv_obj_create(s_screen);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, color(bg), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    return page;
}

static void create_home(void)
{
    s_home = make_page(0x101722);

    // Wi-Fi status on the top center chord; short single line only.
    s_wifi = make_label(s_home, "Wi-Fi: checking", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_wifi, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_wifi, 260);
    lv_label_set_long_mode(s_wifi, LV_LABEL_LONG_DOT);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_MID, 0, 56);

    // One big circular Clare tile, centered on the glass.
    lv_obj_t *clare_btn = lv_button_create(s_home);
    lv_obj_set_size(clare_btn, 200, 200);
    lv_obj_set_style_radius(clare_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(clare_btn, color(0x1B3851), 0);
    lv_obj_set_style_bg_color(clare_btn, color(0x26506F), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(clare_btn, 3, 0);
    lv_obj_set_style_border_color(clare_btn, color(0x3B89B4), 0);
    lv_obj_set_style_border_opa(clare_btn, LV_OPA_70, 0);
    lv_obj_set_style_shadow_width(clare_btn, 0, 0);
    lv_obj_align(clare_btn, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_flex_flow(clare_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clare_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(clare_btn, 6, 0);
    add_action_cb(clare_btn, Action::OpenClare);

    lv_obj_t *avatar = lv_obj_create(clare_btn);
    lv_obj_set_size(avatar, 56, 56);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar, color(0x57B4D9), 0);
    lv_obj_set_style_border_width(avatar, 0, 0);
    lv_obj_set_style_pad_all(avatar, 0, 0);
    lv_obj_clear_flag(avatar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *c = make_label(avatar, "C", 0xFFFFFF, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(c);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_24, 0);

    lv_obj_t *name = make_label(clare_btn, "Clare", 0xFFFFFF, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    make_label(clare_btn, "meeting notes", 0xB8D6E8, LV_TEXT_ALIGN_CENTER);

    // Device demo entry on the bottom center chord.
    lv_obj_t *demo = make_touch_button(s_home, "Device demo", Action::OpenDemo,
                                       150, 44, false, 0x202B3B);
    lv_obj_set_style_border_width(demo, 1, 0);
    lv_obj_set_style_border_color(demo, color(0x394B63), 0);
    lv_obj_align(demo, LV_ALIGN_BOTTOM_MID, 0, -56);
}

static void create_clare(void)
{
    s_clare = make_page(0x101722);

    make_back_button(s_clare);

    lv_obj_t *title = make_label(s_clare, "Clare", 0xF4F7FB, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

    s_status = make_label(s_clare, "Ready when you are", 0x8ED1B2, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_status, 300);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 86);

    s_clare_wifi = make_label(s_clare, "Wi-Fi: checking", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_clare_wifi, &lv_font_montserrat_12, 0);
    lv_obj_align(s_clare_wifi, LV_ALIGN_TOP_MID, 0, 110);

    // Notes card: centered 330-wide, corners verified inside the glass.
    lv_obj_t *notes = lv_obj_create(s_clare);
    lv_obj_set_size(notes, kContentWidth, 200);
    lv_obj_align(notes, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_set_style_radius(notes, 18, 0);
    lv_obj_set_style_bg_color(notes, color(0x182231), 0);
    lv_obj_set_style_bg_opa(notes, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(notes, 1, 0);
    lv_obj_set_style_border_color(notes, color(0x2A3B52), 0);
    lv_obj_set_style_border_opa(notes, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(notes, 14, 0);
    lv_obj_set_scroll_dir(notes, LV_DIR_VER);
    lv_obj_set_flex_flow(notes, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(notes, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    make_label(notes, "LIVE NOTES", 0x70B8D5);
    s_transcript = make_label(notes, "Start a meeting to capture the conversation.", 0xD5E2EE);
    lv_label_set_long_mode(s_transcript, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_transcript, LV_PCT(100));
    lv_obj_set_style_pad_top(s_transcript, 6, 0);
    lv_obj_set_style_text_font(s_transcript, &lv_font_clare_cjk_16, 0);
    make_label(notes, "CLARE", 0xD9AA6A);
    s_answer = make_label(notes, "Ask Clare during or after the meeting.", 0xD5E2EE);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_answer, LV_PCT(100));
    lv_obj_set_style_pad_top(s_answer, 6, 0);
    lv_obj_set_style_text_font(s_answer, &lv_font_clare_cjk_16, 0);

    // Control cluster on the bottom center: circles can sit lower than a
    // rectangle because their lowest point is at center-x.
    lv_obj_t *controls = lv_obj_create(s_clare);
    lv_obj_set_size(controls, kContentWidth, 64);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -38);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_start_btn = make_touch_button(controls, "Start", Action::StartMeeting, 62, 62, true, 0x247C68);
    s_start_label = lv_obj_get_child(s_start_btn, 0);
    s_stop_btn = make_touch_button(controls, "Stop", Action::StopMeeting, 62, 62, true, 0x7C3B4A);
    s_stop_label = lv_obj_get_child(s_stop_btn, 0);
    s_host_btn = make_touch_button(controls, "Ask", Action::ToggleHost, 96, 56, false, 0x38517A);
    s_host_label = lv_obj_get_child(s_host_btn, 0);
    lv_obj_t *refresh_btn = make_touch_button(controls, LV_SYMBOL_REFRESH, Action::Refresh,
                                              52, 52, true, 0x26354A);
    (void)refresh_btn;
}

static void create_demo(void)
{
    s_demo = make_page(0x101722);

    make_back_button(s_demo);

    lv_obj_t *title = make_label(s_demo, "Device", 0xF4F7FB, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *panel = lv_obj_create(s_demo);
    lv_obj_set_size(panel, kContentWidth, 240);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_bg_color(panel, color(0x182231), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, color(0x2A3B52), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_60, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 8, 0);
    make_label(panel, LV_SYMBOL_OK, 0x8ED1B2, LV_TEXT_ALIGN_CENTER);
    make_label(panel, "S3 hardware online", 0xF4F7FB, LV_TEXT_ALIGN_CENTER);
    make_label(panel, "CO5300 466x466 round", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    make_label(panel, "CST9217 touch", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
    make_label(panel, "ES8311 + ES7210 audio", 0x9EB2C9, LV_TEXT_ALIGN_CENTER);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* Called by LVGL event handlers and by locked callers.  It intentionally does
 * not acquire the adapter lock, which avoids recursive-lock deadlocks when a
 * card callback changes pages. */
static void set_page_locked(clare_ui_page_t page)
{
    if (!s_screen) return;
    set_hidden(s_home, page != CLARE_UI_HOME);
    set_hidden(s_clare, page != CLARE_UI_CLARE);
    set_hidden(s_demo, page != CLARE_UI_DEMO);
}

} // namespace

extern "C" void clare_ui_init(const clare_ui_callbacks_t *callbacks)
{
    if (callbacks) s_callbacks = *callbacks;
    if (s_ui_initialized) return;
    if (Lvgl_lock(-1) != ESP_OK) return;
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_size(s_screen, kScreenSize, kScreenSize);
    lv_obj_set_style_bg_color(s_screen, color(0x101722), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_screen_load(s_screen);
    create_home();
    create_clare();
    create_demo();
    s_ui_initialized = true;
    set_page_locked(CLARE_UI_HOME);
    s_meeting_active = false;
    s_host_active = false;
    if (s_stop_btn) lv_obj_add_state(s_stop_btn, LV_STATE_DISABLED);
    Lvgl_unlock();
}

extern "C" void clare_ui_set_page(clare_ui_page_t page)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    set_page_locked(page);
    Lvgl_unlock();
}

extern "C" void clare_ui_set_status(const char *text)
{
    if (!s_status) return;
    if (Lvgl_lock(-1) != ESP_OK) return;
    lv_label_set_text(s_status, text ? text : "");
    Lvgl_unlock();
}

extern "C" void clare_ui_set_transcript(const char *text)
{
    if (!s_transcript) return;
    if (Lvgl_lock(-1) != ESP_OK) return;
    strlcpy(s_transcript_text, text ? text : "", sizeof(s_transcript_text));
    s_transcript_partial[0] = '\0';
    refresh_transcript_locked();
    Lvgl_unlock();
}

extern "C" void clare_ui_reset_transcript(void)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    s_transcript_text[0] = '\0';
    s_transcript_partial[0] = '\0';
    refresh_transcript_locked();
    Lvgl_unlock();
}

extern "C" void clare_ui_append_transcript(const char *text, bool is_final)
{
    if (!text || !text[0] || Lvgl_lock(-1) != ESP_OK) return;
    if (is_final) {
        append_bounded(s_transcript_text, sizeof(s_transcript_text), text);
        append_bounded(s_transcript_text, sizeof(s_transcript_text), "\n");
        s_transcript_partial[0] = '\0';
    } else {
        strlcpy(s_transcript_partial, text, sizeof(s_transcript_partial));
    }
    refresh_transcript_locked();
    Lvgl_unlock();
}

extern "C" void clare_ui_set_answer(const char *text)
{
    if (!s_answer) return;
    if (Lvgl_lock(-1) != ESP_OK) return;
    strlcpy(s_answer_text, text ? text : "", sizeof(s_answer_text));
    s_answer_partial[0] = '\0';
    refresh_answer_locked();
    Lvgl_unlock();
}

extern "C" void clare_ui_reset_answer(void)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    s_answer_text[0] = '\0';
    s_answer_partial[0] = '\0';
    refresh_answer_locked();
    Lvgl_unlock();
}

extern "C" void clare_ui_append_answer(const char *text, bool is_final)
{
    if (!text || !text[0] || Lvgl_lock(-1) != ESP_OK) return;
    if (is_final) {
        append_bounded(s_answer_text, sizeof(s_answer_text), text);
        s_answer_partial[0] = '\0';
    } else {
        strlcpy(s_answer_partial, text, sizeof(s_answer_partial));
    }
    refresh_answer_locked();
    Lvgl_unlock();
}

extern "C" void clare_ui_append_answer_delta(const char *text, bool is_final)
{
    if (!text || !text[0] || Lvgl_lock(-1) != ESP_OK) return;
    append_bounded(s_answer_partial, sizeof(s_answer_partial), text);
    if (is_final) {
        append_bounded(s_answer_text, sizeof(s_answer_text), s_answer_partial);
        s_answer_partial[0] = '\0';
    }
    refresh_answer_locked();
    Lvgl_unlock();
}

extern "C" void clare_ui_set_wifi(const char *text)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    const char *value = text ? text : "Wi-Fi";
    if (s_wifi) lv_label_set_text(s_wifi, value);
    if (s_clare_wifi) lv_label_set_text(s_clare_wifi, value);
    Lvgl_unlock();
}

extern "C" void clare_ui_set_meeting_active(bool active)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    s_meeting_active = active;
    if (s_start_btn) {
        if (active) lv_obj_add_state(s_start_btn, LV_STATE_DISABLED);
        else lv_obj_clear_state(s_start_btn, LV_STATE_DISABLED);
    }
    if (s_stop_btn) {
        if (active) lv_obj_clear_state(s_stop_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(s_stop_btn, LV_STATE_DISABLED);
    }
    if (s_start_label) lv_label_set_text(s_start_label, "Start");
    if (s_stop_label) lv_label_set_text(s_stop_label, "Stop");
    Lvgl_unlock();
}

extern "C" void clare_ui_set_host_active(bool active)
{
    if (Lvgl_lock(-1) != ESP_OK) return;
    s_host_active = active;
    if (s_host_label) lv_label_set_text(s_host_label, active ? "Send" : "Ask");
    Lvgl_unlock();
}
