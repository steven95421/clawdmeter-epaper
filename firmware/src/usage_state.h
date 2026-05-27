#pragma once
#include <Arduino.h>

// Mirrors the JSON payload sent by the Clawdmeter daemon over BLE RX:
//   { "s": 45, "sr": 120, "sa": "Today 1:20PM",
//     "w": 28, "wr": 7200, "wa": "Sun 8:00AM",
//     "st": "allowed", "ok": true }
struct UsageState {
    int     session_pct        = -1;     // s  — session utilisation %
    int     session_reset_min  = -1;     // sr — minutes until session reset
    String  session_reset_at   = "";     // sa — wall-clock reset label
    int     weekly_pct         = -1;     // w  — weekly utilisation %
    int     weekly_reset_min   = -1;     // wr — minutes until weekly reset
    String  weekly_reset_at    = "";     // wa — wall-clock reset label
    String  status             = "";     // st — "allowed" | other
    bool    ok                 = false;  // ok — daemon poll succeeded

    // Bucket reset countdown to match what display.cpp actually prints:
    //   reset_min >= 60   → "reset in Nh"  (hour-resolution)
    //   0 <= reset_min<60 → "reset in Nm"  (minute-resolution)
    //   reset_min < 0     → nothing
    // Two states with different raw minutes but the same bucket render to
    // the same pixels — so we treat them as equal and skip the EPD refresh.
    static int reset_bucket(int reset_min) {
        if (reset_min < 0)  return -1;
        if (reset_min < 60) return reset_min;            // 0..59 minute buckets
        return 60 + (reset_min / 60);                    // 60+ → distinct hour buckets
    }

    bool operator==(const UsageState& o) const {
        // Compare the displayed labels directly so the EPD redraws whenever
        // the wall-clock formatting changes (e.g. "Today" → "Tomorrow"),
        // not just when the underlying minutes cross an hour boundary.
        return session_pct == o.session_pct
            && weekly_pct  == o.weekly_pct
            && session_reset_at == o.session_reset_at
            && weekly_reset_at  == o.weekly_reset_at
            && reset_bucket(session_reset_min) == reset_bucket(o.session_reset_min)
            && reset_bucket(weekly_reset_min)  == reset_bucket(o.weekly_reset_min)
            && status == o.status
            && ok == o.ok;
    }
};
