#include "board.h"
#include <Arduino.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"

// SPI is brought up inside display_hal_init().
// No IO expander, no I2C peripherals are used by this port.
extern "C" void board_init(void) {
    // ---- Release deep-sleep GPIO holds ----
    // When we wake from deep sleep, board_enter_deep_sleep() left the VBAT
    // latch (GPIO17) frozen by gpio_hold_en() and the PWR pin (GPIO18)
    // configured as an RTC ext1 wake source. Release/deinit both before
    // reconfiguring them as normal GPIOs below, or pinMode()/digitalWrite()
    // and the button reads are silently ignored. Both calls are harmless
    // no-ops on a cold boot where nothing was held.
    gpio_hold_dis((gpio_num_t)VBAT_PWR_GPIO);
    rtc_gpio_deinit((gpio_num_t)BTN_PWR_GPIO);

    // ---- Latch the battery power rail ON, as early as possible ----
    // On battery, pressing the PWR side-button only momentarily applies
    // power. We must immediately drive VBAT_PWR HIGH to hold the rail on;
    // otherwise power collapses the instant the button is released and the
    // e-paper keeps the half-drawn splash frame (its bistable nature). This
    // runs before display_hal_init()/display_hal_begin() so the rail is
    // held before the SSD1681 ever receives a byte — mirroring the
    // Waveshare 07_BATT_PWR_Test user_app_init() ordering. Harmless on USB.
    // (The pin is still held HIGH from the previous session on a deep-sleep
    // wake, so re-driving it here is glitch-free.)
    pinMode(VBAT_PWR_GPIO, OUTPUT);
    digitalWrite(VBAT_PWR_GPIO, VBAT_PWR_ON_LEVEL);

    // ---- Boot diagnostics ----
    // Log the reset reason so a brownout loop (repeated ESP_RST_BROWNOUT)
    // is distinguishable from a clean boot when debugging on battery.
    Serial.printf("{\"reset_reason\":%d,\"wake_cause\":%d}\n",
                  (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());
}
