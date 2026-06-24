"""Unit tests for the pure logic in usage_logic.py.

Pure, stdlib-only — no network, Bluetooth, Keychain, or files are touched, and
the BLE daemon module is NOT imported, so this runs with any Python 3 (no venv,
no bleak/httpx):

    python3 daemon/tests/test_logic.py
    # or: python3 -m unittest discover -s daemon/tests
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from usage_logic import (  # noqa: E402
    decide_scan_mode,
    extract_access_token as _extract_access_token,
    pct,
    reset_minutes,
    stable_interval,
    IDLE_LONG_PERIOD,
    IDLE_SHORT_PERIOD,
    PRE_WINDOW,
    STABLE_RATIO,
)


class TestPayloadMath(unittest.TestCase):
    def test_pct_normal(self):
        self.assertEqual(pct("0.45"), 45)
        self.assertEqual(pct("0"), 0)
        self.assertEqual(pct("1"), 100)

    def test_pct_rounds_to_nearest(self):
        self.assertEqual(pct("0.456"), 46)
        self.assertEqual(pct("0.454"), 45)

    def test_pct_bad_input_is_zero(self):
        self.assertEqual(pct(""), 0)
        self.assertEqual(pct("n/a"), 0)

    def test_reset_minutes_future(self):
        self.assertEqual(reset_minutes("7200", 0.0), 120)   # 2 h out
        self.assertEqual(reset_minutes("90", 0.0), 2)       # 90 s rounds to 2 min

    def test_reset_minutes_past_clamps_to_zero(self):
        self.assertEqual(reset_minutes("100", 200.0), 0)    # already past
        self.assertEqual(reset_minutes("0", 0.0), 0)

    def test_reset_minutes_bad_input_is_zero(self):
        self.assertEqual(reset_minutes("", 0.0), 0)
        self.assertEqual(reset_minutes("soon", 0.0), 0)


class TestScanScheduler(unittest.TestCase):
    def test_no_anchor_is_watch(self):
        # No sleep_since anchor -> can't predict a wake -> watch.
        mode, wait = decide_scan_mode([900, 900], None, 1000.0)
        self.assertEqual((mode, wait), ("watch", IDLE_SHORT_PERIOD))

    def test_too_few_samples_is_watch(self):
        self.assertIsNone(stable_interval([900]))
        mode, _ = decide_scan_mode([900], 0.0, 10.0)
        self.assertEqual(mode, "watch")

    def test_inconsistent_gaps_is_watch(self):
        # max/min = 4.5 > STABLE_RATIO -> cadence not trusted.
        self.assertIsNone(stable_interval([900, 200]))
        mode, _ = decide_scan_mode([900, 200], 0.0, 10.0)
        self.assertEqual(mode, "watch")

    def test_confident_and_early_is_quiet(self):
        # Two consistent ~900 s gaps; only 10 s since sleep -> well before the
        # predicted wake (900 - PRE_WINDOW) -> quiet.
        self.assertEqual(stable_interval([900, 900]), 900)
        mode, wait = decide_scan_mode([900, 900], 0.0, 10.0)
        self.assertEqual((mode, wait), ("quiet", IDLE_LONG_PERIOD))

    def test_switches_to_watch_within_pre_window(self):
        # Predicted wake at 900; switch point = 900 - PRE_WINDOW.
        switch = 900 - PRE_WINDOW
        before_mode, _ = decide_scan_mode([900, 900], 0.0, switch - 10)
        after_mode, _ = decide_scan_mode([900, 900], 0.0, switch + 10)
        self.assertEqual(before_mode, "quiet")
        self.assertEqual(after_mode, "watch")

    def test_stable_ratio_boundary_is_inclusive(self):
        # max/min exactly == STABLE_RATIO still counts as consistent (<=).
        self.assertIsNotNone(stable_interval([100.0, 100.0 * STABLE_RATIO]))


class TestTokenExtraction(unittest.TestCase):
    def test_direct_json(self):
        self.assertEqual(
            _extract_access_token('{"accessToken": "sk-ant-oat01-abc123"}'),
            "sk-ant-oat01-abc123",
        )

    def test_nested_json(self):
        self.assertEqual(
            _extract_access_token('{"claudeAiOauth": {"accessToken": "tok-nested"}}'),
            "tok-nested",
        )

    def test_regex_fallback_on_non_json(self):
        self.assertEqual(
            _extract_access_token('garbage before "accessToken": "tok-xyz" after'),
            "tok-xyz",
        )

    def test_raw_token_passthrough(self):
        raw = "sk-ant-oat01-" + "A" * 30
        self.assertEqual(_extract_access_token(raw), raw)

    def test_empty_is_none(self):
        self.assertIsNone(_extract_access_token(""))
        self.assertIsNone(_extract_access_token("   "))

    def test_garbage_is_none(self):
        self.assertIsNone(_extract_access_token("not json, no token here"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
