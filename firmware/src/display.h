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
// `charging` toggles the lightning-bolt icon in the header in place of
// the numeric % (which is unreliable while VBUS is pulling vbat up).
void display_set_battery(int pct, bool charging);

// Deep-sleep helpers. display_wake() powers + inits the panel WITHOUT the
// full-screen clear that display_begin() does (display_render()'s epd_display
// overwrites every pixel anyway, so the clear is just an extra white flash).
// display_sleep() parks the controller and cuts its rail before the MCU
// enters esp_deep_sleep_start(). The panel is bistable, so the last image
// stays on screen the whole time the board is asleep.
void display_wake();
void display_sleep();
