#pragma once
#include <stdint.h>

// RGB565 -> 2-bit BWRY classifier, extracted from display.cpp so it can be
// unit-tested off-target (firmware/test_native/test_pure.cpp) without pulling
// in Arduino.h / epd.h. The EPD_* return codes mirror epd.h and are #ifndef-
// guarded: in the firmware build epd.h has already defined them (this is a
// no-op); the host test includes this header standalone and gets them here.
// MUST stay in sync with epd.h — BLACK 0x0, WHITE 0x1, YELLOW 0x2, RED 0x3.
#ifndef EPD_BLACK
#define EPD_BLACK   0x0
#endif
#ifndef EPD_WHITE
#define EPD_WHITE   0x1
#endif
#ifndef EPD_YELLOW
#define EPD_YELLOW  0x2
#endif
#ifndef EPD_RED
#define EPD_RED     0x3
#endif

// Sentinel returned by classify_rgb565 for the "orange" bar tier. Orange is
// NOT a native JD79667 ink, so it is resolved into a per-pixel YELLOW+RED
// checkerboard in draw_bitmap (which has the x/y needed for the dither). The
// value sits outside the valid 2-bit code range (0..3) so it can never be
// packed into the framebuffer by accident.
#ifndef EPD_DITHER_ORANGE
#define EPD_DITHER_ORANGE 0xFE
#endif

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
