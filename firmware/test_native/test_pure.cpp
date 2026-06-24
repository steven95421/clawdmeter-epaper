// Off-target unit tests for the firmware's pure helpers (no Arduino / LVGL /
// ESP toolchain). Plain g++, no PlatformIO / Unity — this avoids the
// registry.platformio.org fetch that a corporate proxy intercepts, and runs in
// well under a second. Build + run:
//
//   c++ -std=c++17 -Wall -Wextra \
//       -I firmware/src -I firmware/src/boards/waveshare_epaper_154g \
//       firmware/test_native/test_pure.cpp -o /tmp/test_pure && /tmp/test_pure
//
// BOARD_EPAPER_154G is intentionally NOT defined here, so format_reset_time is
// exercised on its raw (non-coarsened) path.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bwry_classify.h"
#include "battery_curve.h"
#include "reset_time_format.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                   \
    } while (0)

// RGB888 -> RGB565, matching the HAL: the classifier expands the 565 value
// back, so feeding 888 here exercises the same quantisation the panel sees.
static uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void test_classify_bwry() {
    // Dark theme backgrounds -> WHITE paper (role inversion).
    CHECK(classify_rgb565(rgb565(0, 0, 0)) == EPD_WHITE);        // BG  #000000
    CHECK(classify_rgb565(rgb565(31, 31, 30)) == EPD_WHITE);     // PANEL ~#1f1f1e
    // Light foreground -> BLACK ink.
    CHECK(classify_rgb565(rgb565(250, 249, 245)) == EPD_BLACK);  // TEXT  #faf9f5
    CHECK(classify_rgb565(rgb565(120, 140, 93)) == EPD_BLACK);   // GREEN low-usage
    // Warm inks survive by hue.
    CHECK(classify_rgb565(rgb565(192, 57, 43)) == EPD_RED);      // RED   high-usage
    CHECK(classify_rgb565(rgb565(220, 160, 40)) == EPD_YELLOW);  // amber accent
    CHECK(classify_rgb565(rgb565(255, 128, 0)) == EPD_DITHER_ORANGE);  // orange tier
}

static void test_battery_curve() {
    CHECK(mv_to_pct(5000) == 100);   // above the top of the curve -> clamp
    CHECK(mv_to_pct(4200) == 100);   // top anchor
    CHECK(mv_to_pct(4100) == 90);    // exact anchor
    CHECK(mv_to_pct(4150) == 95);    // linear interpolation 4100..4200
    CHECK(mv_to_pct(3800) == 50);    // mid anchor
    CHECK(mv_to_pct(3270) == 0);     // bottom anchor
    CHECK(mv_to_pct(3000) == 0);     // below the bottom -> clamp
}

static void test_reset_time_format() {
    char b[48];
    format_reset_time(-1, b, sizeof b);    CHECK(strcmp(b, "---") == 0);
    format_reset_time(0, b, sizeof b);     CHECK(strcmp(b, "Resets in 0m") == 0);
    format_reset_time(45, b, sizeof b);    CHECK(strcmp(b, "Resets in 45m") == 0);
    format_reset_time(90, b, sizeof b);    CHECK(strcmp(b, "Resets in 1h 30m") == 0);
    format_reset_time(1500, b, sizeof b);  CHECK(strcmp(b, "Resets in 1d 1h") == 0);
}

int main() {
    test_classify_bwry();
    test_battery_curve();
    test_reset_time_format();
    printf("%s: %d checks, %d failures\n",
           g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
