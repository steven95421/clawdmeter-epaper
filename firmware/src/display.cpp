#include "display.h"
#include "epd_1in54g.h"

#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSerifBold12pt7b.h>
#include <Fonts/FreeSerifBold9pt7b.h>
#include <Fonts/FreeSerifItalic9pt7b.h>

// ----- 4-colour canvas: Adafruit_GFX subclass writing into a 2bpp
//       packed framebuffer that we then push to the panel verbatim. -----

class EpdCanvas : public Adafruit_GFX {
public:
    EpdCanvas() : Adafruit_GFX(EPD_W, EPD_H) {}

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (x < 0 || x >= EPD_W || y < 0 || y >= EPD_H) return;
        uint8_t c = color & 0x03;
        size_t byte_idx = (size_t)y * EPD_BYTES_PER_ROW + (x >> 2);
        uint8_t shift   = 6 - ((x & 0x03) << 1);
        uint8_t mask    = 0x03 << shift;
        _buf[byte_idx] = (_buf[byte_idx] & ~mask) | (c << shift);
    }

    void fillScreen(uint16_t color) override {
        uint8_t c = color & 0x03;
        uint8_t packed = (c << 6) | (c << 4) | (c << 2) | c;
        memset(_buf, packed, EPD_FRAMEBUF_BYTES);
    }

    const uint8_t* buffer() const { return _buf; }

private:
    uint8_t _buf[EPD_FRAMEBUF_BYTES];
};

static EpdCanvas s_canvas;
static UsageState s_last;
static bool       s_have_last = false;
static int        s_battery_pct = -1;   // latest reading, -1 = unknown
static bool       s_battery_charging = false;
// Logo highlight rotation: increments each time display_render actually
// draws (skipped renders don't count). Means "the logo moved" ≡ "we
// received new state" at a glance.
static int        s_anim_phase = 0;

void display_set_battery(int pct, bool charging) {
    s_battery_pct = pct;
    s_battery_charging = charging;
}

// ---------- helpers ----------

// Tiered colour for "remaining %" of the budget — low remaining = warning.
// (Bar fill & big number now show how much is LEFT, not how much was used,
//  so this mirrors the previous colour_for_pct thresholds.)
static uint16_t colour_for_remaining(int remaining) {
    if (remaining < 0)   return EPD_BLACK;
    if (remaining <= 30) return EPD_RED;     // red = "about to be limited"
    if (remaining <= 60) return EPD_YELLOW;  // yellow = "getting close"
    return EPD_BLACK;
}

// Clawd — Anthropic's official Claude Code mascot crab. Base 20×20 grid
// lifted from ClaudePix (claudepix.vercel.app)'s shared creature-engine.js:
//   0 = transparent  1 = body (#CD7F6A → EPD_RED)  2 = eye (#0f0f0f → EPD_BLACK)
//
// Default eye cells are (row 6, col 7-or-13) and (row 7, col 7-or-13).
// Per-frame animation moves / swaps those cells — taken verbatim from the
// `idle_look_around` preset on ClaudePix so the motion reads as canon.
constexpr int CLAWD_W = 20;
constexpr int CLAWD_H = 20;
static const uint8_t CLAWD_BASE[CLAWD_H][CLAWD_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,0,1,1,2,1,1,1,1,1,2,1,1,0,0,0,0},
    {0,0,0,1,1,1,1,2,1,1,1,1,1,2,1,1,1,1,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1,0,1,0,0},
    {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0},
    {0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0},
    {0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

// Render Clawd at top-left (x,y) on the canvas, scaled `scale` × per cell.
// `phase` cycles the eye animation (6-frame loop).
static void draw_clawd(int x, int y, int scale, int phase) {
    uint8_t frame[CLAWD_H][CLAWD_W];
    memcpy(frame, CLAWD_BASE, sizeof(frame));

    // Patch helpers — write directly into `frame`. Default eye is the 2×2
    // square at rows {6,7} × cols {7,13}. Each frame clears that and
    // repaints fresh eye cells.
    auto clear_eyes = [&]() {
        frame[6][7]  = 1; frame[7][7]  = 1;
        frame[6][13] = 1; frame[7][13] = 1;
    };
    switch (phase % 6) {
        case 1:  // look left — eyes shift to col 6/12
            clear_eyes();
            frame[6][6]  = 2; frame[7][6]  = 2;
            frame[6][12] = 2; frame[7][12] = 2;
            break;
        case 2:  // look right — eyes shift to col 8/14
            clear_eyes();
            frame[6][8]  = 2; frame[7][8]  = 2;
            frame[6][14] = 2; frame[7][14] = 2;
            break;
        case 3:  // look up — single-pixel eyes one row higher
            clear_eyes();
            frame[5][7]  = 2; frame[5][13] = 2;
            break;
        case 4:  // wink — left eye closes, right eye stays open
            frame[6][7] = 1; frame[7][7] = 1;
            break;
        case 5:  // blink — both eyes close
            clear_eyes();
            break;
        default: break;  // 0 = idle
    }

    for (int r = 0; r < CLAWD_H; r++) {
        for (int c = 0; c < CLAWD_W; c++) {
            uint8_t v = frame[r][c];
            if (v == 0) continue;
            uint16_t color = (v == 2) ? EPD_BLACK : EPD_RED;
            if (scale == 1) {
                s_canvas.drawPixel(x + c, y + r, color);
            } else {
                s_canvas.fillRect(x + c * scale, y + r * scale,
                                  scale, scale, color);
            }
        }
    }
}

// Render `text` with a 1-px outline in `outline` colour around `fg`.
// 4 cardinal offsets (not 8-way) — at 9pt FreeSerifBold the diagonal
// passes would smear adjacent strokes into each other.
static void draw_outlined_text(const char* text, int x, int y,
                               uint16_t fg, uint16_t outline) {
    static const int dx[] = { -1,  1,  0,  0 };
    static const int dy[] = {  0,  0, -1,  1 };
    s_canvas.setTextColor(outline);
    for (int i = 0; i < 4; i++) {
        s_canvas.setCursor(x + dx[i], y + dy[i]);
        s_canvas.print(text);
    }
    s_canvas.setTextColor(fg);
    s_canvas.setCursor(x, y);
    s_canvas.print(text);
}

// Plain rectangular bar with a 1-px black border. The fill represents
// "% remaining" — full bar = lots of budget left, empty = depleted.
static void draw_thick_bar(int x, int y, int w, int h, int remaining) {
    if (remaining < 0)   remaining = 0;
    if (remaining > 100) remaining = 100;
    s_canvas.drawRect(x, y, w, h, EPD_BLACK);
    int fill_w = (w - 2) * remaining / 100;
    if (fill_w > 0) {
        s_canvas.fillRect(x + 1, y + 1, fill_w, h - 2, colour_for_remaining(remaining));
    }
}

// 7x11 lightning bolt bitmap (rows top→bottom, MSB = leftmost pixel).
// Hand-traced so the kink in the middle is asymmetric — that's what makes
// it read as ⚡ rather than as a generic zigzag.
static const uint8_t BOLT_BMP[] = {
    0x0C, // ....##.
    0x18, // ...##..
    0x30, // ..##...
    0x60, // .##....
    0xF8, // #####.. ← top of the kink (extends left past the upper diagonal)
    0x3E, // ..##### ← bottom of the kink (extends right past the lower diagonal)
    0x18, // ...##..
    0x30, // ..##...
    0x60, // .##....
    0xC0, // ##.....
    0x80, // #......
};
constexpr int BOLT_W = 7;
constexpr int BOLT_H = 11;

static void draw_bolt(int x, int y, uint16_t color) {
    s_canvas.drawBitmap(x, y, BOLT_BMP, BOLT_W, BOLT_H, color);
}

// Header strip: logo + brand + battery on the right. ~30 px tall.
static void draw_header() {
    // Logo on the left
    // Clawd (Claude Code mascot crab), 2× scaled — content rows 4..16
    // × cols 3..17 lands at screen (4..32, 2..26) with this offset, so
    // the empty top rows of the grid sit off-canvas cleanly.
    draw_clawd(/*x=*/-2, /*y=*/-6, /*scale=*/2, s_anim_phase);
    // "CLAUDE" in red — pushed right to clear the 2× crab sprite.
    s_canvas.setFont(&FreeSerifBold12pt7b);
    s_canvas.setTextColor(EPD_RED);
    s_canvas.setCursor(42, 24);
    s_canvas.print("CLAUDE");

    if (s_battery_pct >= 0) {
        // Battery icon — drawn the same regardless of charging state
        int icon_right = EPD_W - 6;
        int icon_w = 22, icon_h = 11;
        int icon_x = icon_right - icon_w;
        int icon_y = 10;
        uint16_t bc = (s_battery_pct < 20) ? EPD_RED
                    : (s_battery_pct < 50) ? EPD_YELLOW : EPD_BLACK;
        s_canvas.drawRect(icon_x, icon_y, icon_w, icon_h, EPD_BLACK);
        s_canvas.fillRect(icon_right, icon_y + 3, 2, icon_h - 6, EPD_BLACK);
        int fill_w = (icon_w - 2) * s_battery_pct / 100;
        if (fill_w > 0) {
            s_canvas.fillRect(icon_x + 1, icon_y + 1, fill_w, icon_h - 2, bc);
        }

        if (s_battery_charging) {
            // While charging, the LiPo curve is unreliable (terminal voltage
            // is biased high by charge current). Hide the misleading % and
            // draw a yellow ⚡ glyph on the left of the icon instead.
            draw_bolt(icon_x - BOLT_W - 2, icon_y, EPD_YELLOW);
        } else {
            // Resting / discharging — % is meaningful, show it left of the icon.
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", s_battery_pct);
            s_canvas.setFont(&FreeSansBold9pt7b);
            s_canvas.setTextColor(bc);
            int16_t bx, by; uint16_t bw, bh;
            s_canvas.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
            s_canvas.setCursor(icon_x - 4 - bw, 22);
            s_canvas.print(buf);
        }
    }

    // Red rule under the header
    s_canvas.fillRect(0, 30, EPD_W, 2, EPD_RED);
}

// One metric block — label on the left, big % on the right, thick usage
// bar with a small ▼ tick marker above it showing how far the reset
// period has elapsed (so the user can see usage vs time-elapsed at a
// glance: tick ahead of fill = pacing safely, tick behind fill = burning
// the budget fast). period_min = total length of the reset window.
static void draw_metric_block(int top, const char* label, int pct, int reset_min, int period_min,
                              const String& reset_label) {
    // Show "% remaining" rather than "% used" — full bar = budget left.
    int remaining = (pct < 0) ? -1 : (100 - pct);

    // Label and big % share a baseline a bit further down so they don't
    // crowd the divider above them.
    int header_baseline = top + 20;

    // Label (yellow text with a black 1-px outline — pure yellow is too low
    // contrast on white, but outlined it reads cleanly while keeping the
    // accent colour)
    s_canvas.setFont(&FreeSerifBold9pt7b);
    draw_outlined_text(label, 6, header_baseline, EPD_YELLOW, EPD_BLACK);

    // Remaining-% — right-aligned at the same baseline
    s_canvas.setFont(&FreeSansBold12pt7b);
    uint16_t pct_colour = colour_for_remaining(remaining);
    s_canvas.setTextColor(pct_colour);
    char buf[16];
    if (remaining < 0) snprintf(buf, sizeof(buf), "--");
    else               snprintf(buf, sizeof(buf), "%d%%", remaining);
    int16_t bx, by;
    uint16_t bw, bh;
    s_canvas.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
    s_canvas.setCursor(EPD_W - 6 - bw, header_baseline);
    s_canvas.print(buf);

    // Bar geometry — leave room for the tick triangle above
    int bar_x = 6;
    int bar_y = top + 36;
    int bar_w = EPD_W - 12;
    int bar_h = 20;

    // Tick: marks "% of period still remaining" — same direction as the bar
    // (both deplete left-to-right). Tick INSIDE the fill = budget pacing
    // beats time pacing (safe); tick OUTSIDE the fill = budget gone faster
    // than time (burning fast).
    if (period_min > 0 && reset_min >= 0 && reset_min <= period_min) {
        int tick_x = bar_x + 1 + ((bar_w - 2) * reset_min / period_min);
        s_canvas.fillTriangle(
            tick_x,     bar_y - 1,
            tick_x - 4, bar_y - 7,
            tick_x + 4, bar_y - 7,
            EPD_BLACK
        );
    }

    // Bar shows remaining budget
    draw_thick_bar(bar_x, bar_y, bar_w, bar_h, remaining);

    // Reset countdown — prefer the wall-clock label from the daemon
    // ("Resets Today 1:20PM"); fall back to "Nh left" if the daemon hasn't
    // sent one yet (older payload format). Italic serif to read as
    // secondary metadata.
    s_canvas.setFont(&FreeSerifItalic9pt7b);
    s_canvas.setTextColor(EPD_BLACK);
    String text;
    if (reset_label.length()) {
        text = reset_label;
    } else if (reset_min < 0) {
        text = "--";
    } else if (reset_min >= 60) {
        text = String(reset_min / 60) + "h left";
    } else {
        text = String(reset_min) + "m left";
    }
    s_canvas.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    // +14 = bar_bottom + 4 px gap + ascent — tight enough that the italic
    // descender (y/p/g) doesn't fall off the 200 px panel on the weekly
    // block (whose bar_bottom sits at y=182).
    s_canvas.setCursor(EPD_W - 6 - bw, bar_y + bar_h + 14);
    s_canvas.print(text);
}

// ---------- public API ----------

void display_begin() {
    epd_power_on();
    // Fast LUT — fewer flash cycles per refresh (~15s vs ~20s) at the cost
    // of slightly muted colour. Acceptable for the usage dashboard, and the
    // user explicitly complained about full-mode strobing.
    epd_init_fast();
    epd_clear(EPD_WHITE);
}

void display_wake() {
    // Same power-on + fast-LUT init as display_begin(), but WITHOUT epd_clear:
    // the following display_render() rewrites the whole panel, so clearing
    // first would only add a visible white flash and a wasted refresh cycle.
    epd_power_on();
    epd_init_fast();
}

void display_sleep() {
    // Park the controller and drop its rail so the board draws ~nothing while
    // the MCU is in deep sleep. The e-paper image persists (bistable).
    epd_sleep();
    epd_power_off();
}

void display_show_boot(const char* mac) {
    s_canvas.fillScreen(EPD_WHITE);

    draw_header();

    s_canvas.setFont(&FreeSerifBold12pt7b);
    s_canvas.setTextColor(EPD_BLACK);
    s_canvas.setCursor(6, 72);
    s_canvas.print("waiting for");
    s_canvas.setCursor(6, 96);
    s_canvas.print("daemon...");

    s_canvas.setFont(&FreeSerifBold9pt7b);
    s_canvas.setTextColor(EPD_YELLOW);
    s_canvas.setCursor(6, 140);
    s_canvas.print("MAC");

    s_canvas.setFont(&FreeSans9pt7b);
    s_canvas.setTextColor(EPD_BLACK);
    s_canvas.setCursor(6, 158);
    s_canvas.print(mac);

    epd_display(s_canvas.buffer());
    s_have_last = false;
}

void display_force_redraw(const UsageState& s) {
    s_have_last = false;
    display_render(s);
}

void display_render(const UsageState& s) {
    if (s_have_last && s == s_last) return;     // skip identical refresh

    s_canvas.fillScreen(EPD_WHITE);

    draw_header();

    if (!s.ok) {
        s_canvas.setFont(&FreeSerifBold12pt7b);
        s_canvas.setTextColor(EPD_RED);
        s_canvas.setCursor(6, 80);
        s_canvas.print("daemon");
        s_canvas.setCursor(6, 104);
        s_canvas.print("error");
        s_canvas.setFont(&FreeSerifItalic9pt7b);
        s_canvas.setTextColor(EPD_BLACK);
        s_canvas.setCursor(6, 134);
        s_canvas.print(s.status);
    } else {
        // Anthropic publishes session limits in 5-hour windows and weekly
        // limits in a 7-day window. Hard-code the period denominators here
        // so the countdown bar has a stable reference; if Anthropic ever
        // changes the cadence we'll just need to bump these constants.
        constexpr int SESSION_PERIOD_MIN = 5 * 60;
        constexpr int WEEKLY_PERIOD_MIN  = 7 * 24 * 60;
        // session block: top=42, ends ~top+80 → at y≈122
        draw_metric_block(42, "Session", s.session_pct, s.session_reset_min,
                          SESSION_PERIOD_MIN, s.session_reset_at);
        // yellow divider
        s_canvas.fillRect(6, 124, EPD_W - 12, 1, EPD_YELLOW);
        // weekly block: top=126, ends ~204 (text descender 2 px past 200 is fine)
        draw_metric_block(126, "Week", s.weekly_pct, s.weekly_reset_min,
                          WEEKLY_PERIOD_MIN, s.weekly_reset_at);
    }

    epd_display(s_canvas.buffer());
    s_last      = s;
    s_have_last = true;
    // Advance the logo highlight by one petal for the next refresh — visual
    // tick of "we drew something". Wraps at 6 inside draw_claude_mark.
    s_anim_phase++;
}
