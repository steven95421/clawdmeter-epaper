#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port.

Adopted from HermannBjorgvin/Clawdmeter with minor tweaks:
  • Added `from __future__ import annotations` so Apple's /usr/bin/python3
    (3.9) accepts the PEP 604 type hints.
  • Removed REQ_CHAR_UUID subscription gracefully — our firmware (the
    e-paper port) does not implement the 0x0004 "device-requested refresh"
    notify characteristic yet, so the start_notify call falls back to a
    log message and the daemon keeps polling on its own clock.

Polls Anthropic rate-limit headers every 60s and writes JSON to the
ESP32 "Claude Controller" GATT RX characteristic via CoreBluetooth.
"""

from __future__ import annotations

import asyncio
import datetime
import fcntl
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"  # optional, future

POLL_INTERVAL = 300
TICK = 5
SCAN_TIMEOUT = 8.0
# Aggressive rendezvous for the deep-sleep firmware: it only advertises for a
# ~12-30s window every few minutes, so scan near-continuously while
# disconnected (small retry gap) and don't burn 30s on a failed reconnect to a
# board that has gone back to sleep.
SCAN_BACKOFF_MAX = 5
CONNECT_TIMEOUT = 10.0
# Self-heal: macOS CoreBluetooth in a long-lived process often stops returning
# scan results after the Mac sleeps/wakes. If we can't get a successful
# connection for this long, relaunch a fresh process (fresh CBCentralManager).
STALE_RESTART_S = 360
MIN_RESTART_INTERVAL_S = 900
APP_BUNDLE = "/Applications/ClawdmeterDaemon.app"
LOCK_PATH = Path.home() / ".config" / "claude-usage-monitor" / "daemon.lock"

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


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob):
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain():
    try:
        out = subprocess.run(
            ["security", "find-generic-password",
             "-s", KEYCHAIN_SERVICE, "-a", getpass.getuser(), "-w"],
            check=True, capture_output=True, text=True, timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file():
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token():
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def load_cached_address():
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    if (re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr)
            or re.fullmatch(r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr)):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr):
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


_lock_fh = None


def acquire_singleton(timeout=6.0):
    """Hold an exclusive flock so only one daemon runs at a time. Retries
    briefly so a self-restart or the morning restarter can take the lock from
    an instance that is just now exiting."""
    global _lock_fh
    LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    _lock_fh = open(LOCK_PATH, "w")
    deadline = time.time() + timeout
    while True:
        try:
            fcntl.flock(_lock_fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            if time.time() >= deadline:
                return False
            time.sleep(0.3)


def self_restart(reason):
    """Relaunch a brand-new daemon process (fresh CoreBluetooth) and exit.
    Going through the .app bundle keeps the Bluetooth TCC grant; `-n` forces a
    new instance. We hard-exit so our flock releases for the replacement."""
    log(f"Self-restart ({reason}) -- relaunching via {APP_BUNDLE}")
    try:
        subprocess.Popen(["/usr/bin/open", "-n", "-g", APP_BUNDLE])
    except OSError as e:
        log(f"Self-restart spawn failed: {e}")
        return False
    os._exit(0)


async def scan_for_device():
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    try:
        devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    except BleakError as e:
        # BLE permission not yet granted, or hardware off. Stay alive so the
        # user can dismiss the macOS prompt; retry on next iteration.
        log(f"BleakScanner unavailable: {e}")
        return None
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d
    return None


async def poll_api(token):
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

    def hdr(name, default="0"):
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts):
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def reset_label(reset_ts):
        # Pre-formatted wall-clock label so the firmware (no RTC) can show
        # "Today 1:20PM" / "Tomorrow 8:00AM" / "Sun 8:00AM" instead of
        # computing it from a relative minute count.
        try:
            r = float(reset_ts)
        except ValueError:
            return ""
        if r <= now:
            return "soon"
        when = datetime.datetime.fromtimestamp(r).astimezone()
        today = datetime.datetime.now().astimezone().date()
        tomorrow = today + datetime.timedelta(days=1)
        # %-I = hour without leading zero; %p = AM/PM
        clock = when.strftime("%-I:%M%p")
        if when.date() == today:
            return f"Today {clock}"
        if when.date() == tomorrow:
            return f"Tomorrow {clock}"
        return f"{when.strftime('%a')} {clock}"

    def pct(util):
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    return {
        "s":  pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "sa": reset_label(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w":  pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "wa": reset_label(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }


class Session:
    def __init__(self, client):
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data):
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self):
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            # Our firmware doesn't expose this char (yet) — fine, we'll just poll.
            log(f"Refresh subscription unavailable (ok): {e}")

    async def write_payload(self, payload):
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(device_or_address, stop_event):
    # Accept either a BLEDevice (preferred: keeps the scanner's CBCentralManager,
    # avoids a redundant scan during connect that costs ~30s) or a plain address
    # string (fallback when we restored from cache).
    target = device_or_address
    label = getattr(target, "address", target)
    log(f"Connecting to {label}...")
    client = BleakClient(target, timeout=CONNECT_TIMEOUT)
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


async def main():
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args):
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    if not acquire_singleton():
        log("Another daemon instance already holds the lock -- exiting")
        return

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    last_ok_ts = time.time()
    last_restart_ts = time.time()
    backoff = 1

    async def back_off_and_maybe_restart():
        nonlocal backoff, last_restart_ts
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=backoff)
        except asyncio.TimeoutError:
            pass
        backoff = min(backoff * 2, SCAN_BACKOFF_MAX)
        t = time.time()
        if (t - last_ok_ts > STALE_RESTART_S
                and t - last_restart_ts > MIN_RESTART_INTERVAL_S):
            # self_restart() hard-exits on success; we only fall through if the
            # relaunch spawn itself failed, in which case throttle retries.
            self_restart(f"no successful connection for {int(t - last_ok_ts)}s")
            last_restart_ts = time.time()

    while not stop_event.is_set():
        target = load_cached_address()
        if not target:
            device = await scan_for_device()
            if device is not None:
                save_address(device.address)
                target = device
            else:
                log(f"Device not found, retrying in {backoff}s...")
                await back_off_and_maybe_restart()
                continue

        ok = await connect_and_run(target, stop_event)
        if ok:
            last_ok_ts = time.time()
            backoff = 1
        else:
            log("Invalidating cached address")
            SAVED_ADDR_FILE.unlink(missing_ok=True)
            await back_off_and_maybe_restart()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
