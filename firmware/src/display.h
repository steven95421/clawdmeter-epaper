#pragma once
#include "usage_state.h"

void display_begin();
void display_show_boot(const char* mac);
void display_render(const UsageState& s);
// Bypass the "skip if identical" check — used by the BOOT button so the
// user can force a redraw even when the state hasn't changed.
void display_force_redraw(const UsageState& s);

// Latest battery percentage from the ADC sampler in main.cpp. Stored
// module-locally so draw_header() can show it without threading it
// through every render call. -1 = unknown / not sampled yet.
void display_set_battery(int pct);
