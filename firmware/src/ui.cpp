#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t usage_bar_h;
    int16_t panel_hpad;       // make_panel pad_left == pad_right
    int16_t panel_vpad;       // make_panel pad_top  == pad_bottom
    const lv_font_t* usage_title_font;
    const lv_font_t* usage_pct_font;
    const lv_font_t* usage_pill_font;
    const lv_font_t* usage_reset_font;
    const lv_font_t* usage_anim_font;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.margin = 20;
        L.title_y = 30;
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.usage_bar_h = 24;
        L.panel_hpad = 16;
        L.panel_vpad = 12;
        L.usage_title_font = &font_tiempos_56;
        L.usage_pct_font   = &font_styrene_48;
        L.usage_pill_font  = &font_styrene_28;
        L.usage_reset_font = &font_styrene_28;
        L.usage_anim_font  = &font_mono_32;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.height >= 250) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.margin = 20;
        L.title_y = 30;
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.usage_bar_h = 24;
        L.panel_hpad = 16;
        L.panel_vpad = 12;
        L.usage_title_font = &font_tiempos_56;
        L.usage_pct_font   = &font_styrene_48;
        L.usage_pill_font  = &font_styrene_28;
        L.usage_reset_font = &font_styrene_28;
        L.usage_anim_font  = &font_mono_32;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Tiny layout — tuned for 200x200 e-paper (Waveshare 1.54 V2).
        // Keeps every element from the AMOLED layout (logo, battery,
        // both usage panels, rotating animation message) but with
        // shrunk fonts, scaled icons, and tighter spacing so the whole
        // thing fits on a 200px-tall panel. ui_init applies
        // lv_image_set_scale to the logo (~37 %) and battery (~50 %).
        //
        // Vertical budget (200 px total):
        //   y=0..30   top row: logo (30 px), title centred, battery (24 px)
        //   y=34..98  panel 1 (Current) - 64 px
        //   y=102..166 panel 2 (Weekly) - 64 px
        //   y=170..200 footer: rotating animation message
        L.margin = 6;
        // title_y=4 lines up the visible top of the "Usage" /
        // "Bluetooth" header glyphs with the visible-content top of
        // the Claude logo and battery icons (both 0-based on this
        // tier but have a few px of transparent padding at the top of
        // their source bitmaps before the first painted pixel). The
        // styrene_20 line-box at y=4 spans y=4..24 — visible glyphs
        // start around y=6-7, matching where the icons' visible ink
        // begins.
        L.title_y = 4;
        L.content_y = 34;
        L.usage_panel_h = 64;
        L.usage_panel_gap = 4;
        // Within each 64 px panel (1 px pad top+bottom, 62 px usable):
        //   child y=0..28  pct (styrene_28, line_height 28)
        //   child y=32..42 bar (10 px, 4 px gap above)
        //   child y=46..62 reset (styrene_16, line_height 16, 4 px gap)
        L.usage_bar_y = 32;
        L.usage_reset_y = 46;
        L.usage_bar_h = 10;
        L.panel_hpad = 6;
        L.panel_vpad = 1;
        // Match the Bluetooth screen's title size (L.bt_title_font =
        // styrene_20) so the two screens read at the same visual weight.
        L.usage_title_font = &font_styrene_20;
        L.usage_pct_font   = &font_styrene_28;
        // styrene_16 (was 14) so the "Current"/"Weekly" pill reads
        // slightly bigger and balances better against the styrene_28
        // percentage next to it. styrene_16 is the next available
        // size between 14 and 20.
        L.usage_pill_font  = &font_styrene_16;
        L.usage_reset_font = &font_styrene_16;
        // font_mono_18 (DejaVuSansMono) was generated with the U+27xx
        // spinner glyphs and U+2026 ellipsis included; the proportional
        // Styrene fonts are ASCII-only. Using mono here on the tiny
        // tier brings back the original Unicode spinner aesthetic at
        // the cost of a slightly different (monospaced) typeface for
        // the footer line — accepted trade-off documented in the
        // commit message.
        L.usage_anim_font  = &font_mono_18;
        L.bt_info_panel_h = 100;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_styrene_20;
        L.bt_status_font   = &font_styrene_14;
        // styrene_12 (not 14) so "Address: 70:04:1D:DB:CC:89" — 25 chars
        // averaging ~8 px each at styrene_14 — fits on a single line in
        // the 188 px content area. Also used for the "Device:" line and
        // the "Reset Bluetooth" label so they share visual weight.
        L.bt_device_font   = &font_styrene_12;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG
// 154G 4-colour bar gradient (black -> yellow -> orange -> red). A clean
// bright yellow (distinct from the terra-cotta accent) and an "orange" that
// the waveshare_epaper_154g display HAL renders as a YELLOW+RED dither —
// orange is not a native JD79667 ink. Both are only used in 154g-gated code.
#define COL_YELLOW        lv_color_hex(0xf5c518)
#define COL_ORANGE_DITHER lv_color_hex(0xff7f00)

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

// Decorative spinner glyphs: U+00B7 middle dot + U+2722/2733/2736/273B/
// 273D Dingbats stars. Both anim fonts (font_mono_32 on AMOLED,
// font_mono_18 on the tiny tier — DejaVuSansMono in both cases) were
// generated with these codepoints in their glyph range, so the spinner
// renders properly on every board.
static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
#ifndef BOARD_EPAPER_154G
    // 1bpp inverting mono e-paper (waveshare_epaper_154): force a
    // high-luminance text colour so the filled bar lands on the panel-black
    // side of the luminance threshold (real COL_RED/COL_GREEN would invert to
    // near-white). The 4-colour 154G panel SKIPS this and uses true rate
    // colours, which its display classifier maps to RED / YELLOW / BLACK.
    if (L.scr_h < 250) return COL_TEXT;
#endif
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
#ifdef BOARD_EPAPER_154G
    // Coarsen to 15-min steps: a per-minute countdown would re-render the
    // label every minute, and each e-paper full refresh is ~15 s — that reads
    // as constant flashing. Rounding makes the label change ~once per 15 min.
    if (mins > 0) { mins = ((mins + 7) / 15) * 15; if (mins < 15) mins = 15; }
#endif
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    // Panel padding comes from the active tier (tiny tier uses tighter
    // values so the pct + bar + reset fit inside a 64 px panel).
    lv_obj_set_style_pad_left(panel, L.panel_hpad, 0);
    lv_obj_set_style_pad_right(panel, L.panel_hpad, 0);
    lv_obj_set_style_pad_top(panel, L.panel_vpad, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_vpad, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    // Tiny tier (e-paper): give the bar a high-luminance border so the
    // display-HAL inversion renders a clean BLACK outline of the full
    // bar extent on the white panel. Without this the unfilled portion
    // of the bar (COL_BAR_BG, very dark, inverts to panel-white) is
    // invisible and a low-percentage filled bar looks like a tiny smear
    // at the left edge.
    if (L.scr_h < 250) {
        lv_obj_set_style_border_color(bar, COL_TEXT, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_border_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        // Also outline the coloured fill itself in BLACK so the
        // yellow/orange/red indicator reads as a framed segment (the
        // indicator draws over the track, so the MAIN border alone leaves the
        // filled portion un-outlined). The blue-gate classifier folds the
        // fill/border anti-alias fringe into BLACK, so the 2px edge stays
        // solid at the rounded corners.
        lv_obj_set_style_border_color(bar, COL_TEXT, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(bar, 1, LV_PART_INDICATOR);
        lv_obj_set_style_border_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.usage_pill_font, 0);
    // AMOLED: a dim COL_BAR_BG chip with light COL_TEXT on top. On the
    // tiny e-paper tier the 1bpp HAL inverts luminance, so a dark
    // COL_BAR_BG fill would vanish into the white paper — invert the pill
    // (light fill, dark text in LVGL) so it renders as a solid BLACK pill
    // with WHITE "Current"/"Weekly" text on the panel.
#ifdef BOARD_EPAPER_154G
    // 4-colour BWRY panel: a solid RED pill with WHITE "Current"/"Weekly"
    // text. COL_RED classifies to RED ink and COL_BG (black in LVGL) maps to
    // WHITE on the panel, so this is the colourful signature alongside the
    // red logo and title — no muddy yellow involved.
    lv_obj_set_style_bg_color(lbl, COL_RED, 0);
    lv_obj_set_style_text_color(lbl, COL_BG, 0);
    // Black outline around the rounded (LV_RADIUS_CIRCLE) red pill — COL_TEXT
    // classifies to BLACK ink. 2px is enough now that the classifier's blue
    // gate folds the red/border anti-alias fringe into BLACK rather than a
    // yellow halo: the converted fringe effectively thickens the ring inward,
    // so a thin 2px border still reads as a solid, continuous outline.
    lv_obj_set_style_border_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_border_width(lbl, 2, 0);
    lv_obj_set_style_border_opa(lbl, LV_OPA_COVER, 0);
#else
    const bool pill_invert = (L.scr_h < 250);
    lv_obj_set_style_bg_color(lbl, pill_invert ? COL_TEXT : COL_BAR_BG, 0);
    lv_obj_set_style_text_color(lbl, pill_invert ? COL_BG : COL_TEXT, 0);
#endif
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    // Pill padding scales with the font: keep tight on the tiny tier so
    // "Current" / "Weekly" fits next to the percentage without clipping.
    const int hpad = (L.scr_h < 250) ? 8 : 18;
    const int vpad = (L.scr_h < 250) ? 2 : 6;
    lv_obj_set_style_pad_left(lbl, hpad, 0);
    lv_obj_set_style_pad_right(lbl, hpad, 0);
    lv_obj_set_style_pad_top(lbl, vpad, 0);
    lv_obj_set_style_pad_bottom(lbl, vpad, 0);
    return lbl;
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    // AMOLED tiers keep the original +1 nudge (unchanged from before the
    // e-paper port). The tiny tier centres the pill's line-box against the
    // much taller pct glyph so the two read on one baseline:
    //   tiny : pct=28, pill=16+4 pad → offset (28-20)/2 = 4
    const bool pill_tiny = (L.scr_h < 250);
    const int  pill_vpad = pill_tiny ? 2 : 6;
    const int  pct_h     = lv_font_get_line_height(L.usage_pct_font);
    const int  pill_h    = lv_font_get_line_height(L.usage_pill_font)
                         + 2 * pill_vpad;
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0,
                 pill_tiny ? (pct_h - pill_h) / 2 : 1);

    // Bar fills the panel's full content width (panel total minus both
    // sides' padding). The previous "- 32" was hardcoded for AMOLED
    // padding (16+16) and left ~20 px of dead space on the right of
    // the tiny tier (6+6 padding).
    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                       L.content_w - 2 * L.panel_hpad, L.usage_bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, L.usage_title_font, 0);
#ifdef BOARD_EPAPER_154G
    // Red "Usage" title — part of the colourful BWRY signature.
    lv_obj_set_style_text_color(lbl_title, COL_RED, 0);
#else
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
#endif
    // On AMOLED the title is shifted +16 to clear the 80×80 top-left
    // logo overlay. On the tiny tier the logo is scaled to ~30 px and
    // doesn't reach the title, so center cleanly.
    const bool tiny_title = (L.scr_h < 250);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID,
                 tiny_title ? 0 : 16, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    build_pair_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.usage_anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    if (L.scr_h < 250) {
        // Tiny tier uses the proportional 18-px DejaVuSansMono for the
        // anim label (it's the only available font that carries the
        // U+27xx spinner glyphs). The longest messages — e.g.
        // "Flibbertigibbeting" — overflow the panel width at that
        // size, so give the label a fixed width and let LVGL truncate
        // gracefully with its own "..." marker instead of running off
        // the right edge. Width = L.content_w with LV_ALIGN_BOTTOM_MID
        // puts the widget at x=L.margin..(scr_w-L.margin), text
        // centered inside — visually flanked by equal margins on each
        // side.
        lv_obj_set_width(lbl_anim, L.content_w);
        lv_obj_set_style_text_align(lbl_anim, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lbl_anim, LV_LABEL_LONG_MODE_DOTS);

        // Vertically centre the label in the free space between the
        // bottom of the second usage panel and the bottom of the
        // screen. font_mono_18 reports a line height of ~18 px, so
        // half-height ≈ 9.
        const int weekly_bottom = L.content_y + 2 * L.usage_panel_h
                                + L.usage_panel_gap;
        const int free_center   = (weekly_bottom + L.scr_h) / 2;
        const int anim_half_h   = 9;
        const int from_bottom   = L.scr_h - free_center - anim_half_h;
        lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -from_bottom);
    } else {
        lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
    }
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Logo + battery icons. On the tiny tier the source images (80x80
    // logo, 48x48 battery) overwhelm a 200 px panel, so apply LVGL's
    // built-in scaling (256 = 1.0x) to shrink them to ~30 / ~24 px and
    // tuck the logo and battery into the top corners flanking the
    // "Usage" title. Native size on AMOLED tiers.
    //
    // lv_image's scale operates around the image pivot (default
    // center); without overriding the pivot, a scaled 80x80 image
    // renders centered inside its 80x80 bbox with empty padding, so
    // positioning by top-left coordinates doesn't match what's drawn.
    // Pinning pivot to (0,0) anchors the scaled image at the widget's
    // top-left so set_pos coordinates match the visible top-left
    // corner. (At 1.0x scale this is identity; safe to apply on AMOLED.)
    const bool tiny = (L.scr_h < 250);
    const uint32_t logo_scale    = tiny ? 96  : 256;   // 80 -> 30
    const uint32_t battery_scale = tiny ? 128 : 256;   // 48 -> 24
    const int battery_w = (ICON_BATTERY_W * (int)battery_scale) / 256;

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_image_set_pivot(logo_img, 0, 0);
    lv_image_set_scale(logo_img, logo_scale);
    lv_obj_set_pos(logo_img, L.margin, tiny ? 0 : (L.title_y - 10));

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_image_set_pivot(battery_img, 0, 0);
    lv_image_set_scale(battery_img, battery_scale);
    lv_obj_set_pos(battery_img, L.scr_w - battery_w - L.margin,
                   tiny ? 0 : L.title_y);

#ifdef BOARD_EPAPER_154G
    // The logo (terra-cotta crab) and battery glyph are warm-coloured bitmaps
    // that the BWRY classifier would map to muddy YELLOW. For a clean
    // black-on-white dashboard (colour reserved for warnings), recolour both to
    // COL_TEXT — the classifier renders that as BLACK ink. The recolor style
    // persists across lv_image_set_src, so battery state changes stay black.
    lv_obj_set_style_image_recolor(logo_img, COL_RED, 0);
    lv_obj_set_style_image_recolor_opa(logo_img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(battery_img, COL_TEXT, 0);
    lv_obj_set_style_image_recolor_opa(battery_img, LV_OPA_COVER, 0);
#endif
}

// Render one usage metric (percentage label + bar + colour).
//   154g e-paper: show REMAINING budget (100 - utilization), like the
//   standalone firmware — the number + bar DEPLETE as you use Claude, and the
//   colour escalates as little is left (low remaining = warning):
//     remaining > 50 -> GREEN (plenty) ; 20..50 -> AMBER ; <= 20 -> RED.
//   Other boards keep showing UTILISATION % (rises as you use), coloured by
//   pct_color().
static void set_usage_metric(lv_obj_t* pct_lbl, lv_obj_t* bar, float util_pct) {
#ifdef BOARD_EPAPER_154G
    int rem = 100 - (int)(util_pct + 0.5f);
    if (rem < 0) rem = 0; else if (rem > 100) rem = 100;
    lv_label_set_text_fmt(pct_lbl, "%d%%", rem);
    lv_bar_set_value(bar, rem, LV_ANIM_OFF);
    // 4-step heat gradient on remaining budget: BLACK (plenty) -> YELLOW ->
    // ORANGE -> RED (almost out). ORANGE is a YELLOW+RED dither on this
    // 4-colour panel (resolved in the display HAL); the others are native ink.
    lv_color_t c = (rem >= 75) ? COL_TEXT
                 : (rem >= 50) ? COL_YELLOW
                 : (rem >= 25) ? COL_ORANGE_DITHER
                               : COL_RED;
    lv_obj_set_style_bg_color(bar, c, LV_PART_INDICATOR);
#else
    int u = (int)(util_pct + 0.5f);
    lv_label_set_text_fmt(pct_lbl, "%d%%", u);
    lv_bar_set_value(bar, u, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, pct_color(util_pct), LV_PART_INDICATOR);
#endif
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    char buf[48];
    set_usage_metric(lbl_session_pct, bar_session, data->session_pct);
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    set_usage_metric(lbl_weekly_pct, bar_weekly, data->weekly_pct);
    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;

#ifdef BOARD_EPAPER_154G
    // 4-colour full-refresh e-paper (~15 s/refresh): do NOT animate — a
    // rotating spinner/word would thrash the panel. Show a STATIC state word,
    // RED when disconnected so "no data" reads as a warning. Only touch the
    // label when the state actually changes, so a steady state produces no
    // dirty pixels and the display HAL stays idle.
    {
        const int st = !s_ble_connected ? (ble_has_bonds() ? 1 /*disconnected*/
                                                            : 2 /*pairing*/)
                                         : 0 /*tracking*/;
        static int last_st = -1;
        if (st != last_st) {
            last_st = st;
            // Clean scheme: colour reserved for warnings. Tracking/Pairing are
            // BLACK; only Disconnected (a warning) is RED.
            lv_obj_set_style_text_color(lbl_anim, st == 1 ? COL_RED : COL_TEXT, 0);
            lv_label_set_text(lbl_anim, st == 1 ? "Disconnected"
                                      : st == 2 ? "Pairing" : "Tracking");
        }
        return;
    }
#endif

    uint32_t now = lv_tick_get();

    const uint32_t msg_interval     = ANIM_MSG_MS;
    const uint32_t spinner_interval = spinner_ms[anim_spinner_idx];

    if (now - anim_msg_start >= msg_interval) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = ble_has_bonds() ? "Disconnected" : "Pairing";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    // Hide on the splash screen (it's full-bleed pixel art).
    if (current_screen == SCREEN_SPLASH)
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        // Hide the logo on the splash screen (full-bleed); show it elsewhere.
        if (screen == SCREEN_SPLASH)
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    // Connected → usage panels; otherwise → pairing hint. The bottom status
    // line carries the live state word (Connected / Disconnected / Pairing).
    if (usage_group && pair_group) {
        if (s_ble_connected) {
            lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
