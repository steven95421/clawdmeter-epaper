// Clawdmeter (e-paper port) — deep-sleep duty-cycle build
// Hardware: Waveshare ESP32-S3-EPaper-1.54G (200x200 BWRY, JD79667)
//
// Why deep sleep:
//   The board runs on battery at the office while the laptop (the BLE central
//   / daemon) comes and goes. A continuously-advertising radio drains the
//   small LiPo in well under a day, so instead of staying awake we DEEP-SLEEP
//   and wake on a duty cycle:
//
//     wake → sample battery → bring up BLE + advertise → wait a short window
//     for the daemon to connect and push one usage update → refresh the
//     e-paper ONLY if the visible content changed → deep-sleep again.
//
//   Cadence is adaptive:
//     • daemon present → wake every WAKE_INTERVAL_ACTIVE_S (fresh data)
//     • daemon absent  → after IDLE_AFTER_MISSES misses, back off to
//                        WAKE_INTERVAL_IDLE_S to conserve battery overnight /
//                        over the weekend
//     • BOOT button    → ext1 wake for an on-demand refresh
//
//   Each wake is a full reboot (setup() runs again). Only RTC_DATA survives
//   deep sleep, so the render-dedup signature, the miss streak and the charge
//   state live there. The e-paper is bistable: its last image stays on screen
//   the entire time the board is asleep, so a wake with no content change
//   never touches the panel at all.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>
#include "ble_service.h"
#include "display.h"

// --- Pin map (unchanged; verified against the Waveshare wiki + Arduino_3.2.0
//     examples in waveshareteam/ESP32-S3-ePaper-1.54G) ---
//   GPIO 4   BAT_ADC      — VBAT/2 tap (vbat = ADC × 2)
//   GPIO 17  BAT_Control  — soft-power latch; drive HIGH to self-hold the rail
//   GPIO 18  PA_EN        — audio amp enable; drive LOW
//   GPIO 0   BOOT0/button — also the ext1 deep-sleep wake source
constexpr int VBAT_PWR_PIN    = 17;
constexpr int PA_EN_PIN       = 18;
constexpr int BOOT_BUTTON_PIN = 0;
constexpr int BAT_ADC_PIN     = 4;

// --- Duty-cycle tunables ---
constexpr uint32_t WAKE_INTERVAL_ACTIVE_S = 300;    // 5 min while the daemon is around
constexpr uint32_t WAKE_INTERVAL_IDLE_S   = 900;    // 15 min once it's clearly away
constexpr int      IDLE_AFTER_MISSES      = 6;      // ~6 misses (~30 min) → idle cadence
constexpr uint32_t CONNECT_WAIT_MS        = 12000;  // bail this wake if nobody connects
constexpr uint32_t STATE_WAIT_MS          = 30000;  // once connected, wait for the push
constexpr uint32_t POST_STATE_GRACE_MS    = 1200;   // let the write settle before render

// --- Charging detection (no VBUS sense GPIO, so we infer it) ---
// Primary signal is the voltage SLOPE across wakes: while charging, vbat
// climbs between wakes (the charger keeps running even while the MCU sleeps);
// on battery it's flat or slowly falling (deep sleep draws ~µA). The slope
// catches charging of a deeply-drained cell, which sits in the 3.6–3.9V CC
// band for a long time and would fool any fixed threshold ("battery" while
// actually charging). When the slope is flat (e.g. the CV plateau near full,
// where vbat barely moves) we fall back to the absolute thresholds.
constexpr float CHARGING_ENTER_V = 4.08f;   // flat & >= this  → charging (CV plateau)
constexpr float CHARGING_EXIT_V  = 3.98f;   // flat & <= this  → on battery
constexpr float CHARGE_RISE_V    = 0.015f;  // dvbat/wake >=  this → charging
constexpr float CHARGE_FALL_V    = 0.010f;  // dvbat/wake <= -this → on battery

// --- Persisted across deep sleep (RTC slow memory) ---
RTC_DATA_ATTR static uint32_t rtc_render_sig  = 0;       // sig of the on-screen image
RTC_DATA_ATTR static bool     rtc_sig_valid   = false;   // is rtc_render_sig meaningful?
RTC_DATA_ATTR static int      rtc_miss_streak = 0;       // consecutive daemon-absent wakes
RTC_DATA_ATTR static bool     rtc_charging    = false;   // charge-state latch
RTC_DATA_ATTR static float    rtc_last_vbat   = 0.0f;    // last wake's vbat (slope detect)

// --- Usage state pushed by the daemon during the wake window ---
static UsageState    g_state;
static volatile bool g_got_state = false;

static void on_state(const UsageState& s) {
    Serial.printf("[main] new state: s=%d sr=%d w=%d wr=%d st=%s ok=%d\n",
                  s.session_pct, s.session_reset_min,
                  s.weekly_pct,  s.weekly_reset_min,
                  s.status.c_str(), (int)s.ok);
    g_state     = s;
    g_got_state = true;
}

// ---------------------------- battery ----------------------------
// Piecewise-linear LiPo open-circuit-voltage → state-of-charge curve.
static const struct { float v; int pct; } LIPO_CURVE[] = {
    { 4.20f, 100 }, { 4.15f,  95 }, { 4.10f,  90 }, { 4.05f,  82 },
    { 4.00f,  75 }, { 3.95f,  65 }, { 3.90f,  55 }, { 3.85f,  45 },
    { 3.80f,  35 }, { 3.75f,  25 }, { 3.70f,  18 }, { 3.65f,  12 },
    { 3.60f,   8 }, { 3.50f,   4 }, { 3.40f,   1 }, { 3.30f,   0 },
};

static int vbat_to_pct(float vbat) {
    int n = sizeof(LIPO_CURVE) / sizeof(LIPO_CURVE[0]);
    if (vbat >= LIPO_CURVE[0].v) return 100;
    if (vbat <= LIPO_CURVE[n - 1].v) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (vbat <= LIPO_CURVE[i].v && vbat > LIPO_CURVE[i + 1].v) {
            float t = (vbat - LIPO_CURVE[i + 1].v)
                    / (LIPO_CURVE[i].v - LIPO_CURVE[i + 1].v);
            return (int)(LIPO_CURVE[i + 1].pct
                       + t * (LIPO_CURVE[i].pct - LIPO_CURVE[i + 1].pct));
        }
    }
    return 0;
}

struct BatterySample { int pct; float vbat; };

static BatterySample sample_battery() {
    // Calibrated mV via eFuse (analogReadMilliVolts), averaged over 16 reads to
    // suppress ADC jitter. The board halves VBAT through a 1:1 divider.
    constexpr int N = 16;
    long sum_mv = 0;
    for (int i = 0; i < N; i++) sum_mv += analogReadMilliVolts(BAT_ADC_PIN);
    float tap_v = (sum_mv / (float)N) / 1000.0f;
    float vbat  = tap_v * 2.0f;
    return { vbat_to_pct(vbat), vbat };
}

// ----------------------- render-dedup signature -----------------------
// FNV-1a over exactly the fields display.cpp turns into pixels, plus the
// battery bucket and charge state (both shown in the header). Lets us skip the
// ~15s EPD refresh across wakes when nothing visible changed. Mirrors the
// field set in UsageState::operator==.
static uint32_t fnv1a(const void* data, size_t n, uint32_t h) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t compute_sig(const UsageState& s, int batt_bucket, bool charging) {
    uint32_t h  = 2166136261u;
    int sb = UsageState::reset_bucket(s.session_reset_min);
    int wb = UsageState::reset_bucket(s.weekly_reset_min);
    int ok = s.ok ? 1 : 0, ch = charging ? 1 : 0;
    h = fnv1a(&s.session_pct, sizeof(int), h);
    h = fnv1a(&s.weekly_pct,  sizeof(int), h);
    h = fnv1a(&sb, sizeof(int), h);
    h = fnv1a(&wb, sizeof(int), h);
    h = fnv1a(s.session_reset_at.c_str(), s.session_reset_at.length(), h);
    h = fnv1a(s.weekly_reset_at.c_str(),  s.weekly_reset_at.length(),  h);
    h = fnv1a(s.status.c_str(),           s.status.length(),           h);
    h = fnv1a(&ok,          sizeof(int), h);
    h = fnv1a(&batt_bucket, sizeof(int), h);
    h = fnv1a(&ch,          sizeof(int), h);
    return h;
}

// ----------------------------- deep sleep -----------------------------
static void enter_deep_sleep() {
    uint32_t secs = (rtc_miss_streak >= IDLE_AFTER_MISSES)
                  ? WAKE_INTERVAL_IDLE_S : WAKE_INTERVAL_ACTIVE_S;
    Serial.printf("[sleep] deep-sleep for %us (miss_streak=%d)\n", secs, rtc_miss_streak);
    Serial.flush();

    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
    // BOOT button (GPIO0, idle HIGH via the on-board pull-up, LOW when pressed)
    // → on-demand wake. ext1 ANY_LOW is the ESP32-S3 path (ext0 is legacy).
    // Tap, don't hold: GPIO0 is a boot strap, and holding it LOW across the
    // wake-reset would enter download mode instead of running firmware.
    esp_sleep_enable_ext1_wakeup(1ULL << BOOT_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);

    esp_deep_sleep_start();   // never returns
}

void setup() {
    // 1. Latch the battery rail FIRST so a slow boot can't let it collapse.
    pinMode(VBAT_PWR_PIN, OUTPUT); digitalWrite(VBAT_PWR_PIN, HIGH);
    // 2. Audio PA off.
    pinMode(PA_EN_PIN, OUTPUT);    digitalWrite(PA_EN_PIN, LOW);
    // 3. BOOT button readable at runtime (also the ext1 deep-sleep wake source).
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    // 4. Battery ADC: 12-bit, 11 dB attenuation for the full ~0–3.3V tap swing.
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    Serial.begin(115200);
    delay(120);

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool boot_btn  = (cause == ESP_SLEEP_WAKEUP_EXT1);
    bool cold_boot = !(cause == ESP_SLEEP_WAKEUP_TIMER || cause == ESP_SLEEP_WAKEUP_EXT1);
    Serial.printf("\n[wake] cause=%d cold=%d boot_btn=%d miss_streak=%d\n",
                  (int)cause, (int)cold_boot, (int)boot_btn, rtc_miss_streak);

    // --- battery + charge state (voltage slope across wakes; see notes above) ---
    BatterySample b = sample_battery();
    float dv = cold_boot ? 0.0f : (b.vbat - rtc_last_vbat);
    if (cold_boot) {
        rtc_charging = (b.vbat >= CHARGING_ENTER_V);   // no prior sample to slope against
    } else if (dv >=  CHARGE_RISE_V) {
        rtc_charging = true;                            // climbing → charging
    } else if (dv <= -CHARGE_FALL_V) {
        rtc_charging = false;                           // dropping → on battery
    } else if (b.vbat >= CHARGING_ENTER_V) {
        rtc_charging = true;                            // flat & high → CV plateau
    } else if (b.vbat <= CHARGING_EXIT_V) {
        rtc_charging = false;                           // flat & low → on battery
    }   // else flat in the dead band → keep previous state
    rtc_last_vbat = b.vbat;
    Serial.printf("[bat] vbat=%.3fV (d%+.3f) -> %d%% (%s)\n",
                  b.vbat, dv, b.pct, rtc_charging ? "charging" : "battery");
    display_set_battery(b.pct, rtc_charging);
    int batt_bucket = b.pct / 10;

    // --- bring BLE up and start advertising ---
    ble_begin("Claude Controller", on_state);

    bool panel_on = false;
    if (cold_boot) {
        // One-time blank + "waiting for daemon" splash on a real power-on /
        // reset. The panel retains it through every subsequent sleep, so we
        // only ever pay this full clear once.
        display_begin();
        display_show_boot(NimBLEDevice::getAddress().toString().c_str());
        panel_on      = true;
        rtc_sig_valid = false;   // force the first real render after a cold boot
    }

    // --- wake window: wait for the daemon to connect, then push one update ---
    g_got_state = false;
    uint32_t t0 = millis();
    bool connected = false;
    while (true) {
        uint32_t elapsed = millis() - t0;
        if (g_got_state) break;                       // got the push — done waiting
        if (!connected) {
            connected = ble_client_connected();
            if (!connected && elapsed >= CONNECT_WAIT_MS) break;   // nobody home
        }
        if (elapsed >= STATE_WAIT_MS) break;          // connected but no push in time
        delay(50);
    }

    if (g_got_state) {
        rtc_miss_streak = 0;
        delay(POST_STATE_GRACE_MS);
        UsageState s   = g_state;
        uint32_t   sig = compute_sig(s, batt_bucket, rtc_charging);
        bool need_render = boot_btn || !rtc_sig_valid || sig != rtc_render_sig;
        if (need_render) {
            if (!panel_on) { display_wake(); panel_on = true; }
            display_render(s);
            rtc_render_sig = sig;
            rtc_sig_valid  = true;
            Serial.println("[render] EPD updated");
        } else {
            Serial.println("[render] unchanged — EPD left as-is (bistable holds image)");
        }
    } else {
        rtc_miss_streak++;
        Serial.printf("[wake] no daemon push this window (miss_streak=%d)\n", rtc_miss_streak);
    }

    if (panel_on) display_sleep();   // controller sleep + rail off before deep sleep
    enter_deep_sleep();
}

void loop() {
    // Never reached: setup() always ends in esp_deep_sleep_start(), which
    // resets the chip back into setup() on the next wake.
}
