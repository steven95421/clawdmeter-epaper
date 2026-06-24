#include "../../hal/display_hal.h"
#include "board.h"
#include "epd.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "bwry_classify.h"   // classify_rgb565 + EPD_DITHER_ORANGE (extracted; unit-tested)

// 2bpp BWRY framebuffer: 200x200, 4 px/byte, 50-byte rows = 10000 bytes.
#define FB_BYTES   EPD_FRAMEBUF_BYTES

// EPD_DITHER_ORANGE + classify_rgb565 now live in bwry_classify.h (included
// above) so the BWRY classifier can be unit-tested off-target.

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

// classify_rgb565() is defined in bwry_classify.h (included above), extracted
// so the BWRY mapping can be unit-tested off-target.

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
