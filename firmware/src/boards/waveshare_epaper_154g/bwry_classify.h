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

// ---- RGB565 -> 2-bit BWRY 分类器（色相 + 亮度）----
// 灰阶主题值：BG/PANEL/BAR_BG（亮度 0/30/40）-> BLACK；TEXT/DIM/GREEN（亮度 249/175/128）-> WHITE。
// 暖色主题值：AMBER/ACCENT -> YELLOW；RED -> RED；橙色档位 -> EPD_DITHER_ORANGE。
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
        // 真黄的蓝分量低（0xf5c518 -> b=24）；高蓝暖色是 RED 与近白色的过渡像素，不应误判为 YELLOW。
        // 低蓝返回 YELLOW，低绿返回 RED，其余交给亮度分支。
        if (g >= 90 && b < 80) return EPD_YELLOW;
        if (g < 90)            return EPD_RED;
    }
    // 灰阶直接映射面板明暗：亮色（文字/图标）-> 白纸，暗色（背景/面板/轨道）-> 黑墨。
    // 阈值 64 位于主题中最亮背景和最暗前景之间。
    uint32_t lum = ((uint32_t)r * 77 + (uint32_t)g * 150 + (uint32_t)b * 29) >> 8;
    return (lum >= 64) ? EPD_WHITE : EPD_BLACK;
}
