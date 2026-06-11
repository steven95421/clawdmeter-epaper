#include "../../hal/display_hal.h"
#include "board.h"
#include "epd.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

// 2bpp BWRY framebuffer: 200x200, 4 px/byte, 50-byte rows = 10000 bytes.
#define FB_BYTES   EPD_FRAMEBUF_BYTES

// Sentinel returned by classify_rgb565 for the "orange" bar tier. Orange is
// NOT a native JD79667 ink, so it is resolved into a per-pixel YELLOW+RED
// checkerboard in draw_bitmap (which has the x/y needed for the dither). The
// value sits outside the valid 2-bit code range (0..3) so it can never be
// packed into the framebuffer by accident.
#define EPD_DITHER_ORANGE 0xFE

// Full-refresh-only panel: every commit is a ~15 s blocking refresh, so we
// (a) coalesce an LVGL frame (SETTLE_MS), (b) keep a floor between refreshes,
// and (c) only refresh when the packed image ACTUALLY changed (content hash)
// — never on animation/cursor churn that didn't alter committed pixels. The
// hash dedup is the seed of the deep-sleep render-dedup re-grafted in phase 2.
#define SETTLE_MS       600
#define MIN_REFRESH_MS  4000

static uint8_t* framebuf = nullptr;
static bool     dirty = false;
static uint32_t last_flush_ms   = 0;
static uint32_t last_refresh_ms = 0;
// Hash of the image currently on the glass. RTC_DATA_ATTR so it survives MCU
// deep sleep: on a deep-sleep wake the bistable panel still shows the last
// frame, and main.cpp skips the white re-paint (display_hal_begin), so we
// compare a freshly rendered frame against this persisted hash and only spend
// a ~15 s refresh when the image actually changed. Reset to its initializer on
// a cold boot, where display_hal_begin() reseeds it from the white frame.
RTC_DATA_ATTR static uint32_t shown_hash = 0;

// ---- RGB565 -> 2-bit BWRY classifier (hue + luminance, role-inverting) ----
// The shared UI is a DARK theme (light text on near-black bg), natural on
// AMOLED but inverted for paper-white e-paper. We map by ROLE: dark
// backgrounds -> WHITE paper, light text -> BLACK ink. Warm foreground inks
// are caught by hue first so the real RED (high usage / disconnected) and
// YELLOW (medium usage / status accent) survive instead of collapsing to ink.
// Verified against the theme tokens:
//   BG 0x000000 / PANEL 0x1f1f1e / BAR_BG 0x2a2a28  (lum 0/30/40)  -> WHITE
//   TEXT 0xfaf9f5 / DIM 0xb0aea5                     (lum 249/175) -> BLACK
//   GREEN 0x788c5d  (low usage, lum 128)                           -> BLACK
//   AMBER/ACCENT 0xd97757 (medium usage / status)                  -> YELLOW
//   RED 0xc0392b   (high usage / disconnected)                     -> RED
static inline uint8_t classify_rgb565(uint16_t p) {
    uint8_t r = ((p >> 11) & 0x1F) << 3;   // 0..248
    uint8_t g = ((p >>  5) & 0x3F) << 2;   // 0..252
    uint8_t b = ( p        & 0x1F) << 3;   // 0..248
    // Orange sentinel (bar "warning" tier) -> dithered YELLOW+RED. Tight band
    // around 0xff7f00 (HAL-expanded r=248,g=124,b=0) so it never catches the
    // terra-cotta accent (r=217) or the alert red (r=192). Resolved to a
    // per-pixel checkerboard in draw_bitmap, which has the x/y coords.
    if (r >= 240 && g >= 100 && g <= 150 && b < 40)
        return EPD_DITHER_ORANGE;
    // Warm (red-dominant) foreground -> RED, or YELLOW when green is also high.
    if (r >= 100 && r > g + 40 && r > b + 40) {
        // True yellow pigment has LOW blue (0xf5c518 -> b=24). A warm pixel
        // with HIGH blue is a desaturated RED+near-white blend — the anti-alias
        // fringe where the red pill fill meets its light "black" border
        // (COL_TEXT). Without this guard those mid-blends classify YELLOW and
        // paint a yellow halo that breaks the rounded black outline ("gaps").
        // Low blue -> real YELLOW; low green -> RED; otherwise fall through to
        // the luminance branch, where these high-blue fringes resolve to solid
        // BLACK and the outline stays continuous.
        if (g >= 90 && b < 80) return EPD_YELLOW;
        if (g < 90)            return EPD_RED;
    }
    // Greyscale: luminance inversion. Bright (text/icons) -> black ink; dark
    // (bg/panel/bar-track) -> white paper. Threshold 64 sits between the
    // darkest foreground and the lightest background of the theme palette.
    uint32_t lum = ((uint32_t)r * 77 + (uint32_t)g * 150 + (uint32_t)b * 29) >> 8;
    return (lum >= 64) ? EPD_BLACK : EPD_WHITE;
}

static inline void put_px(int px, int py, uint8_t code) {
    uint8_t* byte  = &framebuf[py * EPD_BYTES_PER_ROW + (px >> 2)];
    uint8_t  shift = 6 - ((px & 3) << 1);   // px%4: 0->6, 1->4, 2->2, 3->0
    *byte = (*byte & ~(0x03 << shift)) | ((code & 0x03) << shift);
}

static uint32_t fb_hash(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < FB_BYTES; i++) { h ^= framebuf[i]; h *= 16777619u; }
    return h;
}

void display_hal_init(void) {
    epd_power_on();
    framebuf = (uint8_t*)heap_caps_malloc(FB_BYTES, MALLOC_CAP_INTERNAL);
    if (framebuf) memset(framebuf, 0x55, FB_BYTES);   // all-WHITE (0x01 x4)
    epd_init_fast();
}

void display_hal_begin(void) {
    if (!framebuf) return;
    epd_display(framebuf);            // paint the panel white once at boot
    last_refresh_ms = millis();
    shown_hash      = fb_hash();
}

void display_hal_set_brightness(uint8_t level) { (void)level; }  // no backlight

void display_hal_fill_screen(uint16_t color565) {
    if (!framebuf) return;
    uint8_t c = classify_rgb565(color565);
    if (c == EPD_DITHER_ORANGE) c = EPD_YELLOW;   // can't dither a memset fill
    memset(framebuf, (c << 6) | (c << 4) | (c << 2) | c, FB_BYTES);
    dirty = true;
    last_flush_ms = millis();
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!framebuf || !pixels) return;
    for (int32_t row = 0; row < h; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= EPD_H) continue;
        for (int32_t col = 0; col < w; col++) {
            int32_t px = x + col;
            if (px < 0 || px >= EPD_W) continue;
            uint8_t code = classify_rgb565(pixels[row * w + col]);
            if (code == EPD_DITHER_ORANGE)        // orange = YELLOW+RED checker
                code = ((px + py) & 1) ? EPD_RED : EPD_YELLOW;
            put_px(px, py, code);
        }
    }
    dirty = true;
    last_flush_ms = millis();
}

void display_hal_tick(void) {
    if (!dirty || !framebuf) return;
    uint32_t now = millis();
    if (now - last_flush_ms   < SETTLE_MS)      return;   // coalesce the frame
    // Content dedup BEFORE the refresh floor: an unchanged frame costs nothing,
    // so it must not wait out MIN_REFRESH_MS — on the deep-sleep board that is
    // the difference between ~2 s and ~4.5 s awake on every no-change wake
    // (the nap can't sleep until `dirty` clears). The floor only paces frames
    // that actually differ and will spend a real refresh.
    uint32_t h = fb_hash();
    if (h == shown_hash) { dirty = false; return; }
    if (now - last_refresh_ms < MIN_REFRESH_MS) return;   // floor; keep dirty
    epd_display(framebuf);            // blocking full refresh (~15 s)
    shown_hash      = h;
    last_refresh_ms = millis();
    dirty = false;
}

// True when no committed-frame change is pending — i.e. nothing is queued for
// a refresh and any in-progress refresh has finished (epd_display blocks, so a
// completed tick clears `dirty`). The deep-sleep power-nap in main.cpp uses
// this to know the panel is settled before it sleeps. Board-private.
bool display_hal_idle(void) { return !dirty; }

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // 2bpp packs 4 px/byte, so align X to 4 px (cosmetic with full-frame push,
    // but correct if a per-region path is ever added — NOT 8 px like 1bpp).
    *x1 = (*x1) & ~3;
    *x2 = (*x2) | 3;
    if (*x1 < 0) *x1 = 0;
    if (*x2 >= EPD_W) *x2 = EPD_W - 1;
}
