#pragma once

// Single-cell Li-ion open-circuit-voltage -> percent curve, extracted from
// power.cpp so it can be unit-tested off-target without Arduino. Pure: no
// hardware, no globals.

// Single-cell Li-ion OCV approximation (mV -> percent), high to low,
// linearly interpolated between points. Coarse but honest — not a coulomb
// counter; under load the IR drop reads a few percent low, fine for a
// 5-state icon.
static const struct { int mv; int pct; } LIPO[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 50},
    {3700, 33},  {3600, 20}, {3500, 12}, {3400, 6},  {3300, 2}, {3270, 0},
};

static inline int mv_to_pct(int mv) {
    const int n = sizeof(LIPO) / sizeof(LIPO[0]);
    if (mv >= LIPO[0].mv)     return 100;
    if (mv <= LIPO[n - 1].mv) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (mv <= LIPO[i].mv && mv >= LIPO[i + 1].mv) {
            int dv = LIPO[i].mv  - LIPO[i + 1].mv;
            int dp = LIPO[i].pct - LIPO[i + 1].pct;
            return LIPO[i + 1].pct + (int)((long)(mv - LIPO[i + 1].mv) * dp / dv);
        }
    }
    return 0;
}
