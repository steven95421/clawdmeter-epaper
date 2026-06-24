#pragma once
#include <stddef.h>
#include <stdio.h>

// Reset-countdown label formatter, extracted from ui.cpp so it can be unit-
// tested off-target. On the e-paper board (BOARD_EPAPER_154G) the value is
// coarsened to 15-min steps to avoid a per-minute full refresh; other boards
// render it raw. Pure: writes into the caller's buffer, no globals.
static inline void format_reset_time(int mins, char* buf, size_t len) {
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
