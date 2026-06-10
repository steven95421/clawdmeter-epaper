# macOS LaunchAgents for the Clawdmeter daemon

These run the BLE daemon (`ClawdmeterDaemon.app`, which executes
`daemon/claude_usage_daemon.py`) as a background macOS LaunchAgent, and keep it
alive across crashes / overnight system sleep.

## Files

- **`com.user.clawdmeter-daemon.plist`** — main agent. Launches the app at login
  via `open -g /Applications/ClawdmeterDaemon.app` (`RunAtLoad`, `KeepAlive=false`).
  > Do NOT flip `KeepAlive` to `true`: the `ProgramArguments` is `open`, which
  > exits immediately after launching the app, so `KeepAlive` would respawn it
  > every ~10 s (respawn storm). Auto-heal is handled by the `-restart` agent.

- **`com.user.clawdmeter-daemon-restart.plist`** — health-check resurrector.
  Every 120 s (`StartInterval`, `RunAtLoad`) runs
  `pgrep -f claude_usage_daemon.py >/dev/null 2>&1 || open -g <app>`, i.e.
  relaunches the daemon ONLY if it died. Launching via `open` keeps the app's
  Bluetooth TCC grant. Resurrects within ≤2 min of any death.

## Install on a new machine

1. Install `ClawdmeterDaemon.app` to `/Applications/` (it bundles its own
   `.venv` python + the `claude_usage_daemon.py` from `daemon/`).
2. Copy both plists to `~/Library/LaunchAgents/`.
3. Load them:
   ```sh
   launchctl load ~/Library/LaunchAgents/com.user.clawdmeter-daemon.plist
   launchctl load ~/Library/LaunchAgents/com.user.clawdmeter-daemon-restart.plist
   ```
4. First BLE scan triggers a macOS Bluetooth permission prompt — allow it.

Log: `~/Library/Logs/claude-usage-daemon/stdout.log`.
