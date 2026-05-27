// Clawdmeter (e-paper port)
// Hardware: Waveshare ESP32-S3-EPaper-1.54G (200x200 BWR, SSD1681)
//
// On boot: init e-paper, show "waiting for daemon" + MAC, start BLE GATT.
// On BLE write: parse JSON, render usage on the panel (skips no-op refreshes).

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "ble_service.h"
#include "display.h"

// Soft-power latch — Waveshare's ESP32-S3-ePaper-1.54G uses GPIO17
// (VBAT_PWR) to gate battery power through the ETA6098. PWR button
// (GPIO18) momentarily powers the MCU; firmware must then drive
// VBAT_PWR HIGH to self-hold the rail, otherwise the board dies the
// moment the PWR button is released on battery.
// Verified against waveshareteam/ESP32-S3-ePaper-1.54G/Example/Arduino_3.2.0
// (board_power_bsp.cpp: VBAT_POWER_ON sets level=1).
constexpr int VBAT_PWR_PIN = 17;
constexpr int PWR_BUTTON_PIN  = 18;   // active LOW, hold >=1s to soft-shutdown
constexpr int BOOT_BUTTON_PIN = 0;    // active LOW, short press = redraw EPD
constexpr int BAT_ADC_PIN     = 4;    // VBAT_ADC, VBAT = ADC × 2 via on-board divider

// Latest usage state pushed by the daemon — kept so the BOOT button can
// trigger an EPD redraw without waiting for the next BLE write.
// NOTE: on_state runs on the NimBLE task. The EPD driver does long blocking
// SPI transfers, so we don't render from on_state directly — that races
// with anything else touching the panel from the main loop (boot screen
// during setup(), BOOT button redraw, future status flips, …). Instead we
// just stash the state and flip a "dirty" flag; the main loop picks it up
// and serializes the EPD work.
static UsageState g_last_state;
static bool g_have_state = false;
static volatile bool g_state_dirty = false;
static volatile bool g_force_redraw = false;

static void on_state(const UsageState& s) {
    Serial.printf("[main] new state: s=%d sr=%d w=%d wr=%d st=%s ok=%d\n",
                  s.session_pct, s.session_reset_min,
                  s.weekly_pct,  s.weekly_reset_min,
                  s.status.c_str(), (int)s.ok);
    g_last_state  = s;
    g_have_state  = true;
    g_state_dirty = true;
}

// Soft shutdown: write a "powered off" notice, then drop the VBAT rail.
// On USB power this is a no-op (USB keeps the board up); on battery the
// board really does turn off until PWR is pressed again.
static void soft_shutdown() {
    Serial.println("[pwr] soft shutdown requested — dropping VBAT_PWR");
    display_show_boot("powered off");
    delay(200);
    digitalWrite(VBAT_PWR_PIN, LOW);
    // If we're still alive (USB providing 5V), idle so we don't keep
    // re-triggering the shutdown handler.
    while (true) { delay(1000); }
}

void setup() {
    // Latch battery power FIRST — before Serial / display / BLE — so any
    // boot-time delay can't accidentally let the rail collapse.
    pinMode(VBAT_PWR_PIN, OUTPUT);
    digitalWrite(VBAT_PWR_PIN, HIGH);

    // Both buttons are active LOW on this board; enable the internal pullup
    // so an open-circuit reads HIGH (idle).
    pinMode(PWR_BUTTON_PIN,  INPUT_PULLUP);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    // Battery ADC: 12-bit, 11 dB attenuation so the full ~0-3.3 V swing
    // (after the on-board /2 divider) maps to 0-4095.
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] Clawdmeter e-paper port");
    Serial.println("[boot] VBAT_PWR (GPIO17) latched HIGH");

    display_begin();
    ble_begin("Claude Controller", on_state);

    // Show the MAC on screen so first-time pairing doesn't need a serial monitor.
    String mac = NimBLEDevice::getAddress().toString().c_str();
    display_show_boot(mac.c_str());
}

// Button helpers — simple level-edge tracker with debounce. Active LOW.
struct ButtonState {
    int pin;
    bool was_down;
    uint32_t down_at_ms;
};

static ButtonState pwr_btn  = { PWR_BUTTON_PIN,  false, 0 };
static ButtonState boot_btn = { BOOT_BUTTON_PIN, false, 0 };

static void poll_buttons(uint32_t now) {
    constexpr uint32_t SHORT_PRESS_MAX_MS = 800;
    constexpr uint32_t LONG_PRESS_MIN_MS  = 1000;
    constexpr uint32_t DEBOUNCE_MS        = 30;

    // PWR (GPIO18): long press (>=1s) → soft shutdown.
    bool pwr_down = digitalRead(PWR_BUTTON_PIN) == LOW;
    if (pwr_down && !pwr_btn.was_down) {
        pwr_btn.down_at_ms = now;
        pwr_btn.was_down = true;
    } else if (pwr_down && pwr_btn.was_down) {
        if (now - pwr_btn.down_at_ms >= LONG_PRESS_MIN_MS) {
            soft_shutdown();   // never returns
        }
    } else if (!pwr_down && pwr_btn.was_down) {
        pwr_btn.was_down = false;
    }

    // BOOT (GPIO0): short press → force redraw EPD with most recent state.
    // Just flips the dirty + force flags; the main loop does the actual EPD
    // work so we don't fight on_state for the SPI bus.
    bool boot_down = digitalRead(BOOT_BUTTON_PIN) == LOW;
    if (boot_down && !boot_btn.was_down) {
        boot_btn.down_at_ms = now;
        boot_btn.was_down = true;
    } else if (!boot_down && boot_btn.was_down) {
        uint32_t held = now - boot_btn.down_at_ms;
        boot_btn.was_down = false;
        if (held >= DEBOUNCE_MS && held <= SHORT_PRESS_MAX_MS) {
            if (g_have_state) {
                Serial.println("[boot-btn] force redraw");
                g_state_dirty  = true;
                g_force_redraw = true;
            } else {
                Serial.println("[boot-btn] no state cached yet — nothing to redraw");
            }
        }
    }
}

// Sample VBAT every 5 s, smooth with an average over 16 reads, convert to
// a rough LiPo percentage. Linear fit between 3.30 V (empty) and 4.20 V
// (full) is close enough for a 10 %-bucket display — no point in modelling
// the actual discharge curve when the only consumer is a 1-decimal-digit
// glyph on a panel that only updates every few minutes.
static int g_battery_bucket = -1;
static uint32_t s_last_battery_sample_ms = 0;

static int sample_battery_pct() {
    // Use the Arduino-ESP32 calibrated reading (mV) so we don't have to
    // deal with the ESP32-S3 ADC's non-linear compression at high voltages
    // under 11 dB attenuation. Returns the divider's tap voltage; the
    // board halves VBAT through a 1:1 resistor divider so vbat = tap × 2.
    constexpr int N = 16;
    long sum_mv = 0;
    for (int i = 0; i < N; i++) sum_mv += analogReadMilliVolts(BAT_ADC_PIN);
    float tap_v = (sum_mv / (float)N) / 1000.0f;
    float vbat  = tap_v * 2.0f;
    Serial.printf("[bat] tap=%.3fV vbat=%.3fV\n", tap_v, vbat);
    // Linear LiPo fit: 3.30 V = 0%, 4.20 V = 100%.
    int pct = (int)((vbat - 3.30f) / (4.20f - 3.30f) * 100.0f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static void poll_battery(uint32_t now) {
    constexpr uint32_t SAMPLE_INTERVAL_MS = 5000;
    if (g_battery_bucket >= 0 && now - s_last_battery_sample_ms < SAMPLE_INTERVAL_MS) return;
    s_last_battery_sample_ms = now;
    int pct = sample_battery_pct();
    display_set_battery(pct);
    int bucket = pct / 10;   // 0..10
    if (bucket != g_battery_bucket) {
        Serial.printf("[bat] %d%% (bucket %d -> %d) — flagging redraw\n",
                      pct, g_battery_bucket, bucket);
        g_battery_bucket = bucket;
        // Bucket change is the only time we redraw — otherwise the ADC
        // noise + integer % would refresh the EPD constantly.
        if (g_have_state) {
            g_state_dirty  = true;
            g_force_redraw = true;
        }
    }
}

void loop() {
    ble_loop();
    poll_buttons(millis());
    poll_battery(millis());

    // Drain any pending EPD render. on_state and the BOOT button both just
    // set g_state_dirty; the actual SPI work happens here, on the main task,
    // so there's only ever one writer to the panel.
    if (g_state_dirty) {
        g_state_dirty = false;
        bool force = g_force_redraw;
        g_force_redraw = false;
        UsageState snap = g_last_state;  // copy out (NimBLE task may rewrite)
        if (force) display_force_redraw(snap);
        else       display_render(snap);
    }

    // 5-second heartbeat so the serial monitor isn't silent after boot.
    static uint32_t last_beat = 0;
    uint32_t now = millis();
    if (now - last_beat >= 5000) {
        last_beat = now;
        int n = NimBLEDevice::getServer() ? NimBLEDevice::getServer()->getConnectedCount() : 0;
        Serial.printf("[heartbeat] uptime=%lus  ble_clients=%d\n", now / 1000, n);
    }

    delay(20);   // tighter loop so buttons feel responsive
}
