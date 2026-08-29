#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    float codex_session_pct;       // Codex short window utilization; -1 unavailable
    int codex_session_reset_mins;  // minutes until Codex short window resets
    float codex_weekly_pct;        // Codex weekly utilization; -1 unavailable
    int codex_weekly_reset_mins;   // minutes until Codex weekly window resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
