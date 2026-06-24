"""Pure, dependency-free logic for the Clawdmeter usage daemon.

Everything here is stdlib-only (no bleak / httpx / CoreBluetooth), so it can be
imported and unit-tested without the BLE stack or a venv — see
daemon/tests/test_logic.py. claude_usage_daemon.py imports these symbols.
"""

from __future__ import annotations

import json
import re


# ---- Credentials -----------------------------------------------------------

def extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


# ---- Rate-limit header -> display payload math -----------------------------

def pct(util: str) -> int:
    """Rate-limit utilisation header (e.g. "0.45") -> integer percent (45).
    Non-numeric / missing -> 0."""
    try:
        return int(round(float(util) * 100))
    except ValueError:
        return 0


def reset_minutes(reset_ts: str, now: float) -> int:
    """Reset-timestamp header (unix seconds, as a string) -> whole minutes from
    `now` until reset, clamped at 0 (never negative). Non-numeric -> 0."""
    try:
        r = float(reset_ts)
    except ValueError:
        return 0
    mins = (r - now) / 60.0
    return int(round(mins)) if mins > 0 else 0


# ---- Anchored low-duty scan scheduling -------------------------------------
# Tunables for the away-scan cadence; see claude_usage_daemon.main() for how
# they drive the loop.
IDLE_SHORT_PERIOD = 10    # "watch" wait; dead gap < 15 s timer window (detect + ~3 s connect)
IDLE_LONG_PERIOD  = 45    # "quiet" wait (confident + provably asleep); < 60 s button window
ACTIVE_WAIT       = 2     # s between rapid retries right after a failed connect
PRE_WINDOW        = 90    # s before the predicted wake to switch from "quiet" to "watch"
MIN_LEARN_GAP     = 120   # s: ignore sleep gaps below this (rapid retries / very short)
MAX_LEARN_GAP     = 3600  # s: ignore implausibly long gaps (daemon / Mac downtime)
STABLE_SAMPLES    = 2     # consecutive consistent sleep gaps required to trust the cadence
STABLE_RATIO      = 1.3   # "consistent" = max/min of those recent gaps within this ratio


def stable_interval(gaps: list) -> float | None:
    """Mean of the last STABLE_SAMPLES sleep gaps, but only when they agree
    (max/min within STABLE_RATIO). Returns None when the cadence isn't yet
    trustworthy (too few samples, or inconsistent) — the caller then keeps
    scanning tightly rather than trusting a guessed wake time."""
    if len(gaps) < STABLE_SAMPLES:
        return None
    recent = gaps[-STABLE_SAMPLES:]
    if min(recent) > 0 and max(recent) <= STABLE_RATIO * min(recent):
        return sum(recent) / len(recent)
    return None


def decide_scan_mode(gaps: list, sleep_since: float | None, now: float):
    """Pick the away-scan cadence. Returns (mode, wait_seconds):
      * ("quiet", IDLE_LONG_PERIOD) only when the cadence is trusted AND we're
        provably still asleep — more than PRE_WINDOW before the next predicted
        wake — so a long poll can't straddle the board's ~15 s advert window.
      * ("watch", IDLE_SHORT_PERIOD) otherwise: cold start, unknown / changing
        cadence, approaching a wake, or right after a missed wake.
    Pure function of the learned gaps + the last anchor; unit-tested."""
    interval_est = stable_interval(gaps)
    if (interval_est is not None and sleep_since is not None
            and (now - sleep_since) < interval_est - PRE_WINDOW):
        return "quiet", IDLE_LONG_PERIOD
    return "watch", IDLE_SHORT_PERIOD
