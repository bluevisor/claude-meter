#include "ui.h"
#include "clawd_sprite.h"
#include "theme.h"
#include "display_cfg.h"
#include <lvgl.h>

// LVGL built-in Montserrat (enabled via -DLV_FONT_MONTSERRAT_*=1).
// The Claude Design handoff specifies DM Sans + JetBrains Mono — we
// substitute Montserrat across the board. Sizes are mapped to the
// closest available LVGL preset.
LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_36);
LV_FONT_DECLARE(lv_font_montserrat_48);

#define SCR_W       LCD_WIDTH
#define SCR_H       LCD_HEIGHT
#define MARGIN      12

// ────────────────────── Subscription widgets ──────────────────────
typedef struct {
    lv_obj_t *root;
    lv_obj_t *pct, *pct_sigil;
    lv_obj_t *label, *sublabel;
    lv_obj_t *bar, *bar_fill;
    lv_obj_t *reset_prefix, *reset_value;
} usage_row_t;

static lv_obj_t  *sub_container;
static lv_obj_t  *sub_title, *sub_tag;
static lv_obj_t  *sub_sprite_img;
static usage_row_t sub_5h, sub_7d;

// ────────────────────── API widgets ──────────────────────
static lv_obj_t  *api_container;
static lv_obj_t  *api_title, *api_tag;
static lv_obj_t  *api_sprite_img;
static lv_obj_t  *api_pct, *api_pct_sigil;
static lv_obj_t  *api_token_label, *api_token_count;
static lv_obj_t  *api_segments[20];
static lv_obj_t  *api_reset_prefix, *api_reset_value;
static lv_obj_t  *api_spend_label,   *api_spend_value, *api_spend_of;
static lv_obj_t  *api_burn_label,    *api_burn_value,  *api_burn_delta;

// ────────────────────── Animation widgets ──────────────────────
static lv_obj_t  *anim_container;
static lv_obj_t  *anim_title, *anim_dots;
static lv_obj_t  *anim_clawd_img;
static lv_obj_t  *anim_gen_label, *anim_gen_pct, *anim_gen_bar_bg, *anim_gen_bar_fill;
static lv_obj_t  *anim_ctx_label, *anim_ctx_value, *anim_ctx_bar_bg, *anim_ctx_bar_fill;
static lv_obj_t  *anim_bulb        = nullptr;   // small yellow circle for the lightbulb beat
static lv_obj_t  *anim_thought_dot = nullptr;   // little muted dot for the thinking beat

static screen_t  current_screen = SCREEN_USAGE;
static UsageMode displayed_mode = MODE_UNKNOWN;

enum work_phase_t { PHASE_WORKING, PHASE_THINKING, PHASE_LIGHTBULB };
static work_phase_t cur_phase = PHASE_WORKING;

// Walk-cycle + progress-bar synthetic animation state
static uint8_t   walk_frame    = 1;          // alternates 1↔2 (walkA, walkB)
static uint32_t  walk_last_ms  = 0;
static uint32_t  dot_last_ms   = 0;
static uint8_t   dot_phase     = 0;          // 0..2 chases through the three dots
static float     gen_progress  = 0.05f;
static uint32_t  gen_last_ms   = 0;

// Working-line state. The daemon anchors task_seconds occasionally;
// the firmware advances the displayed timer locally so we get a smooth
// 1s-per-second readout without per-second BLE traffic.
static uint32_t  task_tokens_latched  = 0;
static uint32_t  task_seconds_at_sync = 0;
static uint32_t  task_sync_tick_ms    = 0;
static bool      task_active_latched  = false;

static void render_working_line(void) {
    uint32_t s = task_seconds_at_sync;
    if (task_active_latched) {
        // Add wall-clock seconds since the last sync. lv_tick_get() is
        // monotonic and millisecond-granular.
        uint32_t delta_ms = lv_tick_get() - task_sync_tick_ms;
        s += delta_ms / 1000;
    }
    char buf[32];
    char tok[16];
    // Spinner: while a turn is mid-flight but no output tokens have
    // landed yet, show a rotating ASCII glyph instead of "0".
    if (task_active_latched && task_tokens_latched == 0) {
        static const char* SPIN_FRAMES = "|/-\\";
        uint32_t spin_idx = (lv_tick_get() / 120) & 3;
        tok[0] = SPIN_FRAMES[spin_idx];
        tok[1] = '\0';
    } else if (task_tokens_latched < 1000) {
        snprintf(tok, sizeof(tok), "%u",     (unsigned)task_tokens_latched);
    } else if (task_tokens_latched < 100000) {
        snprintf(tok, sizeof(tok), "%.1fk",  task_tokens_latched / 1000.0);
    } else {
        snprintf(tok, sizeof(tok), "%.0fk",  task_tokens_latched / 1000.0);
    }
    if (s < 60)        snprintf(buf, sizeof(buf), "%s   %us",     tok, (unsigned)s);
    else if (s < 3600) snprintf(buf, sizeof(buf), "%s   %u:%02u", tok, (unsigned)(s / 60), (unsigned)(s % 60));
    else               snprintf(buf, sizeof(buf), "%s   %uh%02u", tok, (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
    if (anim_gen_pct) lv_label_set_text(anim_gen_pct, buf);
}

// ────────────────────── Helpers ──────────────────────

static lv_obj_t* make_container(lv_obj_t* parent) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, SCR_W, SCR_H);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_color(c, THEME_BG, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// A bare positioning <div> with no chrome (no bg, no border).
static lv_obj_t* make_pane(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color,
                            const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

// Build a row matching SubScreen's <Row> in screens.jsx, scaled up for
// the 1.3" panel viewed through the prism — bigger fonts, thicker bar,
// more breathing room.
//
// Row stack (88 px tall):
//   y=0:  % left (M48) + label/sublabel right (M18/M14)
//   y=54: 10-px bar
//   y=68: "Resets in <value>" (M14 / M18)
static void build_usage_row(lv_obj_t* parent, int top, lv_color_t color, lv_color_t color_deep,
                            const char* label, const char* sublabel, usage_row_t* out) {
    out->root = make_pane(parent, MARGIN, top, SCR_W - 2 * MARGIN, 88);

    out->pct = make_label(out->root, &lv_font_montserrat_48, color, "--");
    lv_obj_align(out->pct, LV_ALIGN_TOP_LEFT, 0, -6);

    out->pct_sigil = make_label(out->root, &lv_font_montserrat_28, color_deep, "%");
    lv_obj_align_to(out->pct_sigil, out->pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 1, -4);

    out->label = make_label(out->root, &lv_font_montserrat_18, THEME_TEXT, label);
    lv_obj_align(out->label, LV_ALIGN_TOP_RIGHT, 0, 4);

    out->sublabel = make_label(out->root, &lv_font_montserrat_14, THEME_TEXT_MUTED, sublabel);
    lv_obj_align_to(out->sublabel, out->label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);

    out->bar = make_pane(out->root, 0, 52, SCR_W - 2 * MARGIN, 10);
    lv_obj_set_style_bg_color(out->bar, THEME_BAR_BG, 0);
    lv_obj_set_style_bg_opa(out->bar, LV_OPA_COVER, 0);

    out->bar_fill = make_pane(out->root, 0, 52, 0, 10);
    lv_obj_set_style_bg_color(out->bar_fill, color, 0);
    lv_obj_set_style_bg_opa(out->bar_fill, LV_OPA_COVER, 0);

    out->reset_prefix = make_label(out->root, &lv_font_montserrat_14, THEME_TEXT_MUTED, "Resets in");
    lv_obj_set_pos(out->reset_prefix, 0, 68);

    out->reset_value = make_label(out->root, &lv_font_montserrat_18, THEME_TEXT, "---");
    lv_obj_align_to(out->reset_value, out->reset_prefix, LV_ALIGN_OUT_RIGHT_MID, 6, -1);
}

static void set_usage_row(usage_row_t* r, int pct, int reset_mins) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_label_set_text_fmt(r->pct, "%d", pct);
    lv_obj_align_to(r->pct_sigil, r->pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 1, -4);

    int bar_w = (SCR_W - 2 * MARGIN) * pct / 100;
    lv_obj_set_size(r->bar_fill, bar_w, 10);

    // Session mode (reset_mins < 0) has no "resets in" semantic — hide both
    // the prefix and the value so the row reads cleanly.
    if (reset_mins < 0) {
        lv_obj_add_flag(r->reset_prefix, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->reset_value,  LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(r->reset_prefix, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(r->reset_value,  LV_OBJ_FLAG_HIDDEN);
    char buf[24];
    if (reset_mins < 60)         snprintf(buf, sizeof(buf), "%dm", reset_mins);
    else if (reset_mins < 1440)  snprintf(buf, sizeof(buf), "%dh %dm", reset_mins / 60, reset_mins % 60);
    else                         snprintf(buf, sizeof(buf), "%dd %dh", reset_mins / 1440, (reset_mins % 1440) / 60);
    lv_label_set_text(r->reset_value, buf);
}

// Title bar across Sub / API screens. Title is the model name (e.g.
// "Opus 4.7 1M"); the MAX/API tag sits to the right of the title.
static void build_title_bar(lv_obj_t* parent, const char* tag_text,
                            lv_obj_t** out_title, lv_obj_t** out_sprite,
                            lv_obj_t** out_tag) {
    *out_title = make_label(parent, &lv_font_montserrat_24, THEME_TEXT, "Usage");
    lv_obj_set_pos(*out_title, MARGIN, 4);

    *out_sprite = lv_image_create(parent);
    lv_image_set_src(*out_sprite, clawd_sprite_tiny());
    lv_obj_align(*out_sprite, LV_ALIGN_TOP_RIGHT, -MARGIN, 6);

    *out_tag = make_label(parent, &lv_font_montserrat_14, THEME_TEXT_MUTED, tag_text);
    lv_obj_align_to(*out_tag, *out_sprite, LV_ALIGN_OUT_LEFT_MID, -4, 0);
}

// ────────────────────── Subscription screen ──────────────────────

static void init_subscription_screen(lv_obj_t* parent) {
    sub_container = make_container(parent);
    build_title_bar(sub_container, "MAX", &sub_title, &sub_sprite_img, &sub_tag);

    // Layout (240 vertical):
    //   y=  4..36   title bar (M28 + sprite + tag)
    //   y= 42..130  row 1: Current  (88 px tall, see build_usage_row)
    //   y=134       hairline (1 px)
    //   y=140..228  row 2: Weekly
    //   y=228..240  bottom margin
    build_usage_row(sub_container, 42,  THEME_ACCENT, THEME_ACCENT_DEEP,
                    "Current", "5h window", &sub_5h);

    lv_obj_t* hr = make_pane(sub_container, MARGIN, 134, SCR_W - 2 * MARGIN, 1);
    lv_obj_set_style_bg_color(hr, THEME_HAIRLINE, 0);
    lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);

    build_usage_row(sub_container, 140, THEME_AMBER,  THEME_AMBER_DEEP,
                    "Weekly",  "7d window", &sub_7d);
}

// ────────────────────── API screen ──────────────────────

static void init_api_screen(lv_obj_t* parent) {
    api_container = make_container(parent);
    build_title_bar(api_container, "API", &api_title, &api_sprite_img, &api_tag);

    // y=42  big % (M48) + Tokens / token-count right column
    api_pct = make_label(api_container, &lv_font_montserrat_48, THEME_ACCENT, "--");
    lv_obj_set_pos(api_pct, MARGIN, 42);

    api_pct_sigil = make_label(api_container, &lv_font_montserrat_36, THEME_ACCENT_DEEP, "%");
    lv_obj_align_to(api_pct_sigil, api_pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);

    api_token_label = make_label(api_container, &lv_font_montserrat_18, THEME_TEXT, "Tokens");
    lv_obj_align(api_token_label, LV_ALIGN_TOP_RIGHT, -MARGIN, 48);

    api_token_count = make_label(api_container, &lv_font_montserrat_14, THEME_TEXT_MUTED, "0 / 0");
    lv_obj_align_to(api_token_count, api_token_label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);

    // y=112: 12-px segmented bar (10 segments — wider/thicker cells at this scale)
    {
        int avail = SCR_W - 2 * MARGIN;
        int gap = 3;
        int seg_count = 10;
        int seg_w = (avail - gap * (seg_count - 1)) / seg_count;
        for (int i = 0; i < 20; i++) api_segments[i] = nullptr;
        int x = MARGIN;
        for (int i = 0; i < seg_count; i++) {
            lv_obj_t* s = make_pane(api_container, x, 112, seg_w, 12);
            lv_obj_set_style_bg_color(s, THEME_BAR_BG, 0);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            api_segments[i] = s;
            x += seg_w + gap;
        }
    }

    // y=130: "Resets in 11d 4h"
    api_reset_prefix = make_label(api_container, &lv_font_montserrat_14, THEME_TEXT_MUTED, "Resets in");
    lv_obj_set_pos(api_reset_prefix, MARGIN, 130);

    api_reset_value = make_label(api_container, &lv_font_montserrat_18, THEME_TEXT, "---");
    lv_obj_align_to(api_reset_value, api_reset_prefix, LV_ALIGN_OUT_RIGHT_MID, 6, -1);

    // Hairline at y=160
    lv_obj_t* hr = make_pane(api_container, MARGIN, 160, SCR_W - 2 * MARGIN, 1);
    lv_obj_set_style_bg_color(hr, THEME_HAIRLINE, 0);
    lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);

    // y=168..232: Spend (left) + Burn (right)
    api_spend_label = make_label(api_container, &lv_font_montserrat_14, THEME_TEXT_MUTED, "Spend");
    lv_obj_set_pos(api_spend_label, MARGIN, 168);

    api_spend_value = make_label(api_container, &lv_font_montserrat_28, THEME_TEXT, "$0.00");
    lv_obj_align_to(api_spend_value, api_spend_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    api_spend_of = make_label(api_container, &lv_font_montserrat_14, THEME_TEXT_MUTED, "of $0");
    lv_obj_align_to(api_spend_of, api_spend_value, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    api_burn_label = make_label(api_container, &lv_font_montserrat_14, THEME_TEXT_MUTED, "Burn/min");
    lv_obj_align(api_burn_label, LV_ALIGN_TOP_RIGHT, -MARGIN, 168);

    api_burn_value = make_label(api_container, &lv_font_montserrat_28, THEME_ACCENT, "--");
    lv_obj_align_to(api_burn_value, api_burn_label, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);

    api_burn_delta = make_label(api_container, &lv_font_montserrat_14, THEME_AMBER, "--");
    lv_obj_align_to(api_burn_delta, api_burn_value, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
}

// ────────────────────── Animation screen ──────────────────────

static void init_animation_screen(lv_obj_t* parent) {
    anim_container = make_container(parent);

    // Header removed: the figure gets the headroom now. anim_title /
    // anim_dots are kept as hidden stubs so the rest of the code that
    // touches them stays compiling.
    anim_title = make_label(anim_container, &lv_font_montserrat_14, THEME_TEXT, "");
    anim_dots  = make_label(anim_container, &lv_font_montserrat_14, THEME_ACCENT, "");
    lv_obj_add_flag(anim_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(anim_dots,  LV_OBJ_FLAG_HIDDEN);

    // Clawd, centered horizontally — room left at the top so the head
    // doesn't clip against the bezel; later ticks bob/sway around this.
    anim_clawd_img = lv_image_create(anim_container);
    lv_image_set_src(anim_clawd_img, clawd_sprite_large(0));
    lv_obj_align(anim_clawd_img, LV_ALIGN_TOP_MID, 0, 20);

    // (The lightbulb beat now replaces the entire figure with a yellow
    // bulb sprite — no separate overlay widget needed.)
    anim_bulb = nullptr;

    // Thought dot — a small muted bubble that drifts up during the
    // thinking beat. Hidden by default.
    anim_thought_dot = lv_obj_create(anim_container);
    lv_obj_remove_style_all(anim_thought_dot);
    lv_obj_set_size(anim_thought_dot, 10, 10);
    lv_obj_set_style_radius(anim_thought_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(anim_thought_dot, THEME_TEXT_MUTED, 0);
    lv_obj_set_style_bg_opa(anim_thought_dot, LV_OPA_COVER, 0);
    lv_obj_align(anim_thought_dot, LV_ALIGN_TOP_MID, 28, 14);
    lv_obj_add_flag(anim_thought_dot, LV_OBJ_FLAG_HIDDEN);

    // No "GENERATING" label — just the tokens + elapsed time, centered
    // and large so it reads at a glance from across the desk.
    anim_gen_label = make_label(anim_container, &lv_font_montserrat_14, THEME_TEXT_MUTED, "");
    lv_obj_add_flag(anim_gen_label, LV_OBJ_FLAG_HIDDEN);

    anim_gen_pct = make_label(anim_container, &lv_font_montserrat_36, THEME_ACCENT, "--");
    lv_obj_align(anim_gen_pct, LV_ALIGN_TOP_MID, 0, 156);

    // Progress bar removed; stubs kept for back-compat with existing refs.
    anim_gen_bar_bg   = make_pane(anim_container, MARGIN, 180, SCR_W - 2 * MARGIN, 0);
    anim_gen_bar_fill = make_pane(anim_container, MARGIN, 180, 0, 0);
    lv_obj_add_flag(anim_gen_bar_bg,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(anim_gen_bar_fill, LV_OBJ_FLAG_HIDDEN);

    // CONTEXT bar — bumped to 18pt for legibility from across the desk.
    anim_ctx_label = make_label(anim_container, &lv_font_montserrat_18, THEME_TEXT_MUTED, "CONTEXT");
    lv_obj_set_pos(anim_ctx_label, MARGIN, 202);

    anim_ctx_value = make_label(anim_container, &lv_font_montserrat_18, THEME_TEXT, "--");
    lv_obj_align(anim_ctx_value, LV_ALIGN_TOP_RIGHT, -MARGIN, 202);

    anim_ctx_bar_bg = make_pane(anim_container, MARGIN, 224, SCR_W - 2 * MARGIN, 8);
    lv_obj_set_style_bg_color(anim_ctx_bar_bg, THEME_BAR_BG, 0);
    lv_obj_set_style_bg_opa(anim_ctx_bar_bg, LV_OPA_COVER, 0);

    anim_ctx_bar_fill = make_pane(anim_container, MARGIN, 224, 0, 8);
    lv_obj_set_style_bg_color(anim_ctx_bar_fill, THEME_GREEN, 0);
    lv_obj_set_style_bg_opa(anim_ctx_bar_fill, LV_OPA_COVER, 0);
}

// ────────────────────── Public API ──────────────────────

void ui_init(void) {
    clawd_sprite_init();

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, THEME_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_subscription_screen(scr);
    init_api_screen(scr);
    init_animation_screen(scr);

    lv_obj_add_flag(api_container,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(anim_container, LV_OBJ_FLAG_HIDDEN);
}

static void apply_mode_visibility(void) {
    if (current_screen != SCREEN_USAGE) return;
    if (displayed_mode == MODE_API) {
        lv_obj_add_flag(sub_container,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(api_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(sub_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(api_container,   LV_OBJ_FLAG_HIDDEN);
    }
}

static void format_token_count(uint64_t n, char* buf, size_t len) {
    if (n < 1000ULL)            snprintf(buf, len, "%llu",   (unsigned long long)n);
    else if (n < 1000000ULL)    snprintf(buf, len, "%.0fk",  n / 1000.0);
    else if (n < 1000000000ULL) snprintf(buf, len, "%.1fM",  n / 1000000.0);
    else                        snprintf(buf, len, "%.1fB",  n / 1000000000.0);
}

// Health gradient for the context bar — picks a color band based on
// current_pct (0..100). Bands per design: <10 green, <20 dark green,
// <40 blue, <60 yellow, <80 orange, ≥80 red.
static lv_color_t context_color_for(int pct) {
    if (pct < 10) return lv_color_hex(0x00C060);  // bright green
    if (pct < 20) return lv_color_hex(0x008040);  // dark green
    if (pct < 40) return lv_color_hex(0x1090E0);  // blue
    if (pct < 60) return lv_color_hex(0xFFD000);  // yellow
    if (pct < 80) return lv_color_hex(0xFF7000);  // orange
    return        lv_color_hex(0xE61500);          // red
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    bool mode_changed = (displayed_mode != data->mode);
    displayed_mode = data->mode;
    if (mode_changed) apply_mode_visibility();

    // Title: model name when known, else fall back to "Usage". The
    // MAX/API badge sits to its right.
    const char* title_text = data->model_label[0] ? data->model_label : "Usage";
    lv_label_set_text(sub_title, title_text);
    lv_label_set_text(api_title, title_text);
    lv_label_set_text(sub_tag, "MAX");
    lv_label_set_text(api_tag, "API");
    lv_obj_clear_flag(sub_tag, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(api_tag, LV_OBJ_FLAG_HIDDEN);

    if (data->mode == MODE_API) {
        int pct = 0;
        if (data->tokens_quota > 0) {
            double p = (double)data->tokens_used * 100.0 / (double)data->tokens_quota;
            pct = (int)(p + 0.5);
            if (pct > 100) pct = 100;
        }
        lv_label_set_text_fmt(api_pct, "%d", pct);
        lv_obj_align_to(api_pct_sigil, api_pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);

        // Segmented bar fill (10 cells; last 2 turn amber when filled).
        int filled = (pct * 10 + 50) / 100;
        const int hot_start = 8;
        for (int i = 0; i < 10; i++) {
            if (!api_segments[i]) continue;
            lv_color_t c = THEME_BAR_BG;
            if (i < filled) c = (i >= hot_start) ? THEME_AMBER : THEME_ACCENT;
            lv_obj_set_style_bg_color(api_segments[i], c, 0);
        }

        char buf[32], buf2[32];
        format_token_count(data->tokens_used,  buf,  sizeof(buf));
        format_token_count(data->tokens_quota, buf2, sizeof(buf2));
        char count[48];
        if (data->tokens_quota > 0) snprintf(count, sizeof(count), "%s / %s", buf, buf2);
        else                        snprintf(count, sizeof(count), "%s used", buf);
        lv_label_set_text(api_token_count, count);

        if (data->api_reset_mins < 0)            snprintf(buf, sizeof(buf), "---");
        else if (data->api_reset_mins < 60)      snprintf(buf, sizeof(buf), "%dm", data->api_reset_mins);
        else if (data->api_reset_mins < 1440)    snprintf(buf, sizeof(buf), "%dh %dm",
                                                          data->api_reset_mins / 60,
                                                          data->api_reset_mins % 60);
        else                                     snprintf(buf, sizeof(buf), "%dd %dh",
                                                          data->api_reset_mins / 1440,
                                                          (data->api_reset_mins % 1440) / 60);
        lv_label_set_text(api_reset_value, buf);

        // Spend $X.XX / of $Y
        int spent  = data->dollars_spent_cents;
        int budget = data->dollars_budget_cents;
        char money[16];
        snprintf(money, sizeof(money), "$%d.%02d", spent / 100, spent % 100);
        lv_label_set_text(api_spend_value, money);
        if (budget > 0) snprintf(money, sizeof(money), "of $%d", budget / 100);
        else            snprintf(money, sizeof(money), "no cap");
        lv_label_set_text(api_spend_of, money);

        // Burn / min
        format_token_count((uint64_t)data->burn_per_min, buf, sizeof(buf));
        lv_label_set_text(api_burn_value, buf);
        if (data->burn_pct_change >= 0) snprintf(buf, sizeof(buf), "↑ %d%% avg", data->burn_pct_change);
        else                            snprintf(buf, sizeof(buf), "↓ %d%% avg", -data->burn_pct_change);
        lv_label_set_text(api_burn_delta, buf);
        lv_obj_set_style_text_color(api_burn_delta,
            data->burn_pct_change >= 0 ? THEME_AMBER : THEME_GREEN, 0);
    } else {
        set_usage_row(&sub_5h, (int)(data->session_pct + 0.5f), data->session_reset_mins);
        set_usage_row(&sub_7d, (int)(data->weekly_pct  + 0.5f), data->weekly_reset_mins);
    }

    // Context bar on the animation screen (shared across modes)
    if (data->ctx_max > 0) {
        char buf[32];
        // Format the cap as "1M" when we're on the million-token variant,
        // otherwise as "<n>k" — much more readable than "1000k".
        if (data->ctx_max >= 1000000ULL) {
            snprintf(buf, sizeof(buf), "%.1fk / %lluM",
                     data->ctx_used / 1000.0,
                     (unsigned long long)(data->ctx_max / 1000000));
        } else {
            snprintf(buf, sizeof(buf), "%.1fk / %lluk",
                     data->ctx_used / 1000.0,
                     (unsigned long long)(data->ctx_max / 1000));
        }
        lv_label_set_text(anim_ctx_value, buf);
        int avail = SCR_W - 2 * MARGIN;
        int w = (int)((double)data->ctx_used * (double)avail / (double)data->ctx_max);
        if (w > avail) w = avail;
        lv_obj_set_size(anim_ctx_bar_fill, w, 8);
        // Health gradient: green → red as the window fills up.
        int pct = (int)(data->ctx_used * 100ULL / data->ctx_max);
        lv_obj_set_style_bg_color(anim_ctx_bar_fill, context_color_for(pct), 0);
    } else {
        lv_label_set_text(anim_ctx_value, "--");
        lv_obj_set_size(anim_ctx_bar_fill, 0, 8);
    }

    // Latch the working-line state. The label itself is rendered on
    // every animation tick from task_anchor_* so the timer advances
    // one second per second locally, without per-second BLE traffic.
    task_tokens_latched   = data->task_tokens;
    task_seconds_at_sync  = data->task_seconds;
    task_sync_tick_ms     = lv_tick_get();
    task_active_latched   = data->session_active;
    render_working_line();

    // Phase swap drives which animation sequence runs and which overlay
    // (lightbulb glow / thought dot) is shown.
    work_phase_t new_phase = PHASE_WORKING;
    if (strcmp(data->phase, "thinking")  == 0) new_phase = PHASE_THINKING;
    if (strcmp(data->phase, "lightbulb") == 0) new_phase = PHASE_LIGHTBULB;
    if (new_phase != cur_phase) {
        cur_phase = new_phase;
        walk_last_ms = 0;  // re-arm so the new sequence starts immediately
    }
    if (anim_bulb) {
        if (cur_phase == PHASE_LIGHTBULB) lv_obj_clear_flag(anim_bulb, LV_OBJ_FLAG_HIDDEN);
        else                              lv_obj_add_flag(anim_bulb,   LV_OBJ_FLAG_HIDDEN);
    }
    if (anim_thought_dot) {
        if (cur_phase == PHASE_THINKING) lv_obj_clear_flag(anim_thought_dot, LV_OBJ_FLAG_HIDDEN);
        else                             lv_obj_add_flag(anim_thought_dot,   LV_OBJ_FLAG_HIDDEN);
    }

    // Auto-switch: when Claude Code is mid-turn, jump to the working/anim
    // view; when it goes idle, return to the usage view. We only react to
    // *transitions* so a touch toggle isn't fought against on every push.
    static bool last_active_known = false;
    static bool last_active = false;
    bool act = data->session_active;
    if (!last_active_known || act != last_active) {
        last_active_known = true;
        last_active = act;
        if (act && current_screen != SCREEN_SPLASH) {
            ui_show_screen(SCREEN_SPLASH);
        } else if (!act && current_screen == SCREEN_SPLASH) {
            ui_show_screen(SCREEN_USAGE);
        }
    }
}

// Frame indices: 0,1 idle walk-cycle  2 blink  3 squint
//                4 look-left  5 look-right  6 wide/surprise  7 sleepy

// Default "working" cycle — mixes everyday expressions.
static const uint8_t ANIM_WORKING[] = {
    0, 1, 0, 1, 0, 1, 2,
    0, 1, 4, 0, 5, 0,
    0, 1, 0, 1, 3,
    0, 1, 0, 1, 6,
    0, 1, 7, 7,
    0, 1, 2,
};
// "Thinking" cycle — heavier on sleepy/squint/look-around, fewer steps,
// longer holds. Reads as focused / contemplative.
static const uint8_t ANIM_THINKING[] = {
    7, 7, 3, 3, 4, 4, 5, 5,
    7, 3, 0, 7, 3, 2,
};
// "Lightbulb" cycle — the figure is replaced by a yellow bulb sprite
// that twinkles for a few beats. Frames 8 (plain) / 9 (with shine).
static const uint8_t ANIM_LIGHTBULB[] = {
    8, 9, 8, 9, 8, 9, 8, 9,
};

struct anim_phase_def_t {
    const uint8_t* seq;
    uint16_t       len;
    uint16_t       interval_ms;
};
static const anim_phase_def_t PHASE_ANIM[] = {
    /* PHASE_WORKING   */ { ANIM_WORKING,   sizeof(ANIM_WORKING),   320 },
    /* PHASE_THINKING  */ { ANIM_THINKING,  sizeof(ANIM_THINKING),  520 },
    /* PHASE_LIGHTBULB */ { ANIM_LIGHTBULB, sizeof(ANIM_LIGHTBULB), 220 },
};

// Body bob (vertical) + sway (horizontal) — the sway reads as a gentle
// arm-swing relative to the body without changing the silhouette.
static const int8_t BOB_OFFSETS[]  = { 0, -2, -3, -2, 0, 2, 3, 2 };
static const int8_t SWAY_OFFSETS[] = { 0, -1, -2, -2, -1, 0, 1, 2, 2, 1 };
#define BOB_LEN  (sizeof(BOB_OFFSETS)  / sizeof(BOB_OFFSETS[0]))
#define SWAY_LEN (sizeof(SWAY_OFFSETS) / sizeof(SWAY_OFFSETS[0]))
#define ANIM_BASE_Y 20

void ui_tick_anim(void) {
    if (current_screen != SCREEN_SPLASH) return;
    uint32_t now = lv_tick_get();

    // Refresh the working line frequently while active so the spinner
    // animates smoothly. Cheap — just one lv_label_set_text call.
    static uint32_t last_render_ms = 0;
    if (task_active_latched && now - last_render_ms >= 150) {
        last_render_ms = now;
        render_working_line();
    }

    const anim_phase_def_t& def = PHASE_ANIM[cur_phase];

    if (now - walk_last_ms >= def.interval_ms) {
        walk_last_ms = now;
        static uint16_t seq_idx = 0;
        static uint16_t bob_idx = 0;
        static uint16_t sway_idx = 0;
        // Wrap inside the active phase's sequence length so a phase
        // swap doesn't index past the end of a shorter array.
        seq_idx  = (seq_idx + 1)  % def.len;
        bob_idx  = (bob_idx + 1)  % BOB_LEN;
        sway_idx = (sway_idx + 1) % SWAY_LEN;
        lv_image_set_src(anim_clawd_img, clawd_sprite_large(def.seq[seq_idx]));
        // Lightbulb makes the figure hop a touch higher; thinking holds
        // it still (no bob/sway) so it reads as concentrating.
        int sway = SWAY_OFFSETS[sway_idx];
        int bob  = BOB_OFFSETS[bob_idx];
        if (cur_phase == PHASE_THINKING) { sway = 0; bob = 0; }
        else if (cur_phase == PHASE_LIGHTBULB) { bob -= 2; }
        lv_obj_align(anim_clawd_img, LV_ALIGN_TOP_MID,
                     sway, ANIM_BASE_Y + bob);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(sub_container,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(api_container,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(anim_container, LV_OBJ_FLAG_HIDDEN);

    switch (screen) {
    case SCREEN_USAGE:
        if (displayed_mode == MODE_API) lv_obj_clear_flag(api_container, LV_OBJ_FLAG_HIDDEN);
        else                            lv_obj_clear_flag(sub_container, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_SPLASH:
        lv_obj_clear_flag(anim_container, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_BLUETOOTH:
        // Status screen retired in the redesign — fall through to usage.
        if (displayed_mode == MODE_API) lv_obj_clear_flag(api_container, LV_OBJ_FLAG_HIDDEN);
        else                            lv_obj_clear_flag(sub_container, LV_OBJ_FLAG_HIDDEN);
        break;
    default: break;
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
}

void ui_cycle_screen(void) {
    // BOOT-button: toggle between usage and animation.
    ui_show_screen(current_screen == SCREEN_SPLASH ? SCREEN_USAGE : SCREEN_SPLASH);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                 ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) { return current_screen; }

// BLE status used to drive the (now-retired) Status screen. Keep the symbol
// alive so main.cpp still links; ignore the parameters.
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)state; (void)name; (void)mac;
}

void ui_update_battery(int percent, bool charging) {
    (void)percent; (void)charging;
}
