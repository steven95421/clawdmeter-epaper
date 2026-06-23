#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

from __future__ import annotations  # 3.9-compat: lazy annotations (X | None)

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
import traceback
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 4.0  # short scans cut BT-radio occupancy; a board advert is seen in <1s

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
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


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)
_cb_fastpath_broken = False  # set once if the macOS system-connected fast-path is unusable


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Claude Controller', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    global _cb_fastpath_broken
    if _cb_fastpath_broken:
        return None  # fast-path already known unusable on this bleak; scan handles it

    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # e.g. bleak 1.x dropped CentralManagerDelegate.wait_until_ready
        log(f"CoreBluetooth system-connected fast-path unavailable ({e}); "
            f"using scan-by-name for the rest of this run")
        _cb_fastpath_broken = True
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    payload = {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }
    return payload


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                token = read_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
                    if payload is not None:
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    # --- Anchored low-duty scan scheduling ---------------------------------
    # The board deep-sleeps and only advertises briefly per wake (~15 s on the
    # periodic timer wake, ~60 s on a PWR-button / cold-boot wake), every
    # NAP_WAKE_INTERVAL_MIN minutes. Continuous scanning to catch that window
    # monopolises the Mac's single Bluetooth radio and makes other BT devices
    # (e.g. a mouse) stutter. So:
    #   * ANCHOR on each CONFIRMED disconnect (sleep_since): the board just
    #     entered deep sleep, so the next timer wake is ~= sleep_since + interval.
    #   * Learn the sleep interval, but TRUST it only once the last few sleeps
    #     are CONSISTENT (confident). When confident AND provably still asleep,
    #     scan only lightly ("quiet", IDLE_LONG_PERIOD ~ once/min — enough to
    #     catch a ~60 s button advertisement); radio is mostly free.
    #   * Otherwise scan tightly ("watch", IDLE_SHORT_PERIOD) so the dead gap
    #     stays under the ~15 s timer window and we detect AND connect in time
    #     (~3 s). "watch" covers the approach to a wake, cold start, an
    #     unconfident / changing cadence, a board that's been gone a while, and
    #     the cycle right after any missed wake.
    # We RE-ANCHOR on every contact, so the phase never free-runs (a missed wake
    # just leaves us in "watch", which catches the next wake). A 45 s "quiet"
    # poll is used ONLY when we're confident the board can't be waking, so it can
    # never straddle a 15 s timer window. The interval is recomputed from the
    # recent window, so it tracks the true cadence in BOTH directions; a one-off
    # button / missed-wake gap breaks consistency -> we fall back to "watch",
    # which only costs duty, never a missed wake.
    IDLE_SHORT_PERIOD = 10    # "watch" wait; dead gap < 15 s timer window (detect + ~3 s connect)
    IDLE_LONG_PERIOD  = 45    # "quiet" wait (confident + provably asleep); < 60 s button window
    ACTIVE_WAIT       = 2     # s between rapid retries right after a failed connect
    PRE_WINDOW        = 90    # s before the predicted wake to switch from "quiet" to "watch"
    MIN_LEARN_GAP     = 120   # s: ignore sleep gaps below this (rapid retries / very short)
    MAX_LEARN_GAP     = 3600  # s: ignore implausibly long gaps (daemon / Mac downtime)
    STABLE_SAMPLES    = 2     # consecutive consistent sleep gaps required to trust the cadence
    STABLE_RATIO      = 1.3   # "consistent" = max/min of those recent gaps within this ratio

    sleep_since: float | None = None   # monotonic time of last confirmed disconnect (sleep entry)
    gaps: list = []                    # recent sleep durations (disconnect -> next wake), recent last
    announced: str | None     = None   # log mode transitions once, not per scan
    skip_addr: str | None     = None   # macOS: a peripheral to skip for one cycle
    prev_mono = prev_wall = None       # for macOS system-sleep (monotonic-freeze) detection
    while not stop_event.is_set():
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None

        if not target:
            now = loop.time()
            # macOS system sleep freezes the monotonic clock while wall-clock
            # advances; the learned phase is then meaningless, so drop the anchor
            # and re-acquire (keep the learned gaps — the cadence itself is valid).
            wall = time.time()
            if prev_mono is not None and (wall - prev_wall) - (now - prev_mono) > 30:
                sleep_since = None
            prev_mono, prev_wall = now, wall

            # Trust the interval only when the last STABLE_SAMPLES sleeps agree;
            # recomputing from the recent window tracks the cadence both ways.
            interval_est = None
            if len(gaps) >= STABLE_SAMPLES:
                recent = gaps[-STABLE_SAMPLES:]
                if min(recent) > 0 and max(recent) <= STABLE_RATIO * min(recent):
                    interval_est = sum(recent) / len(recent)

            if (interval_est is not None and sleep_since is not None
                    and (now - sleep_since) < interval_est - PRE_WINDOW):
                mode, wait = "quiet", IDLE_LONG_PERIOD   # confident + provably asleep; radio mostly free
            else:
                mode, wait = "watch", IDLE_SHORT_PERIOD  # approach / cold start / unsure / board gone
            if mode != announced:
                announced = mode
                log(f"Board asleep — light scan every {IDLE_LONG_PERIOD}s; radio mostly free"
                    if mode == "quiet"
                    else f"Watching for a wake — scan every {IDLE_SHORT_PERIOD}s")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=wait)
            except asyncio.TimeoutError:
                pass
            continue

        # Found the board: record the sleep-duration sample, then connect.
        now = loop.time()
        prev_mono = prev_wall = None         # reset the sleep-detector baseline after any contact
        if sleep_since is not None:
            gap = now - sleep_since          # observed disconnect -> wake duration
            if MIN_LEARN_GAP <= gap <= MAX_LEARN_GAP:
                gaps.append(gap)
                del gaps[:-5]                # keep only the recent few samples
        announced = None

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if ok:
            sleep_since = loop.time()        # clean session ended -> board asleep now; anchor here
        else:
            if sys.platform == "darwin":
                # Stale CoreBluetooth handle: skip it next cycle so the scan
                # fallback is reachable. Board is awake right now — retry fast.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=ACTIVE_WAIT)
            except asyncio.TimeoutError:
                pass


if __name__ == "__main__":
    exit_code = 0
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
    except Exception:
        # os._exit() below skips normal finalization, so Python would never
        # print this traceback on its own — log it ourselves and exit non-zero
        # so an unhandled crash is visible (in the log and to anything watching
        # the exit code) instead of looking like a clean shutdown.
        traceback.print_exc()
        exit_code = 1
    finally:
        # bleak's CoreBluetooth backend services callbacks on a separate
        # dispatch-queue thread ("bleak.corebluetooth"). On Python 3.14, normal
        # interpreter finalization on the main thread (atexit + OpenSSL
        # OPENSSL_cleanup) can race with a still-in-flight CoreBluetooth KVO
        # callback on that thread, which then touches torn-down runtime state →
        # use-after-free SIGSEGV on shutdown (a "Python quit unexpectedly" crash
        # report on every exit; with a KeepAlive launchd job, a respawn storm).
        # We have nothing to flush back (every log line is print(flush=True)), so
        # skip finalization entirely and _exit() at the kernel level — no
        # userspace cleanup means no window for the BT thread to fault.
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(exit_code)
