#include "display.h"
#include "epd_1in54g.h"

#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

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

// Anthropic-style asterisk mark: three lines through (cx,cy) at 60° spacing,
// thickened to ~3 px so it reads on the 200-DPI panel.
static void draw_claude_mark(int cx, int cy, int r, uint16_t color) {
    static const float angles_deg[3] = { 0.0f, 60.0f, 120.0f };
    for (int i = 0; i < 3; i++) {
        float a = angles_deg[i] * (float)M_PI / 180.0f;
        int dx = (int)(r * cosf(a));
        int dy = (int)(r * sinf(a));
        // 3 parallel lines for thickness
        s_canvas.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, color);
        s_canvas.drawLine(cx - dx,     cy - dy + 1, cx + dx,     cy + dy + 1, color);
        s_canvas.drawLine(cx - dx + 1, cy - dy,     cx + dx + 1, cy + dy,     color);
    }
    // Small filled centre so the joins look solid
    s_canvas.fillCircle(cx, cy, 3, color);
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

// Header strip: logo + brand. ~30 px tall.
static void draw_header() {
    // Logo on the left
    draw_claude_mark(/*cx=*/14, /*cy=*/16, /*r=*/10, EPD_RED);
    // "CLAUDE" in red
    s_canvas.setFont(&FreeSansBold12pt7b);
    s_canvas.setTextColor(EPD_RED);
    s_canvas.setCursor(34, 24);
    s_canvas.print("CLAUDE");
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

    // Label (yellow, small bold)
    s_canvas.setFont(&FreeSansBold9pt7b);
    s_canvas.setTextColor(EPD_YELLOW);
    s_canvas.setCursor(6, header_baseline);
    s_canvas.print(label);

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
    // sent one yet (older payload format).
    s_canvas.setFont(&FreeSans9pt7b);
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
    // +18 = bar_bottom + 6 px gap + ~12 px ascent, so glyphs clear the bar
    s_canvas.setCursor(EPD_W - 6 - bw, bar_y + bar_h + 18);
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

void display_show_boot(const char* mac) {
    s_canvas.fillScreen(EPD_WHITE);

    draw_header();

    s_canvas.setFont(&FreeSansBold12pt7b);
    s_canvas.setTextColor(EPD_BLACK);
    s_canvas.setCursor(6, 72);
    s_canvas.print("waiting for");
    s_canvas.setCursor(6, 96);
    s_canvas.print("daemon...");

    s_canvas.setFont(&FreeSansBold9pt7b);
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
        s_canvas.setFont(&FreeSansBold12pt7b);
        s_canvas.setTextColor(EPD_RED);
        s_canvas.setCursor(6, 80);
        s_canvas.print("daemon");
        s_canvas.setCursor(6, 104);
        s_canvas.print("error");
        s_canvas.setFont(&FreeSans9pt7b);
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
}
