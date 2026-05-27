#!/bin/bash
# macOS installer for the Clawdmeter (e-paper) BLE daemon.
#
# Why this script is shaped the way it is:
#
#   macOS TCC attributes Bluetooth permission to the *bundle identity* of
#   the requesting process. That means we can't just point launchd at
#   /usr/bin/python3 — bare Python silently re-execs into Apple's
#   Python.framework Python.app whose Info.plist has no
#   NSBluetoothAlwaysUsageDescription, so TCC denies without even
#   prompting. The workaround is to build a real .app bundle that
#   embeds a patched Python interpreter inside its own Contents/Resources,
#   so the BLE call attributes to com.user.clawdmeter-daemon.
#
# The end state is:
#
#   /Applications/ClawdmeterDaemon.app/
#     Contents/
#       Info.plist                  (BLE usage description, LSUIElement)
#       MacOS/
#         ClawdmeterDaemon          tiny C launcher (real Mach-O)
#       Resources/
#         claude_usage_daemon.py
#         .venv/                    embedded Python interpreter + bleak/httpx
#                                   (Python binary patched + re-signed so
#                                    macOS doesn't re-route through the
#                                    framework's Python.app trampoline)
#
#   ~/Library/LaunchAgents/com.user.clawdmeter-daemon.plist
#                                   RunAtLoad → /usr/bin/open -g <bundle>

set -euo pipefail

LABEL="com.user.clawdmeter-daemon"
BUNDLE="/Applications/ClawdmeterDaemon.app"
LAUNCH_AGENT="$HOME/Library/LaunchAgents/$LABEL.plist"
DAEMON_SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR="$HOME/Library/Logs/claude-usage-daemon"

echo "=== Clawdmeter installer (macOS, .app bundle) ==="
echo "Bundle    : $BUNDLE"
echo "Source    : $DAEMON_SRC_DIR"
echo "LaunchAgent: $LAUNCH_AGENT"

# ---------- prerequisites ---------------------------------------------------

SYSTEM_PYTHON="/usr/bin/python3"
if [[ ! -x "$SYSTEM_PYTHON" ]]; then
    echo "Apple's /usr/bin/python3 not found. Run 'xcode-select --install' first."
    exit 1
fi

if ! command -v cc >/dev/null; then
    echo "C compiler not found. Run 'xcode-select --install' first."
    exit 1
fi

# Resolve the actual Python interpreter & framework dylib so we can copy +
# patch the binary into the bundle. We deliberately use Apple's CLT 3.9
# because its pyexpat / Keychain stack works out of the box; Homebrew's
# python@3.13/3.14 currently has a libexpat ABI mismatch.
FRAMEWORK_BIN=$("$SYSTEM_PYTHON" - <<'PY'
import os, sys
# The framework lives at /Library/Developer/CommandLineTools/Library/Frameworks/Python3.framework/Versions/<X>/
# and the real interpreter is at Resources/Python.app/Contents/MacOS/Python (not bin/python3.X which trampolines).
base = sys.base_prefix
candidate = os.path.join(base, "Resources", "Python.app", "Contents", "MacOS", "Python")
print(candidate if os.path.exists(candidate) else "")
PY
)
FRAMEWORK_DYLIB=$("$SYSTEM_PYTHON" -c 'import os, sys; print(os.path.join(sys.base_prefix, "Python3"))')

if [[ -z "$FRAMEWORK_BIN" || ! -x "$FRAMEWORK_BIN" ]]; then
    echo "Could not locate the real Python interpreter at $FRAMEWORK_BIN"
    exit 1
fi
if [[ ! -e "$FRAMEWORK_DYLIB" ]]; then
    # framework dylib is sometimes referenced as `Python3` directly
    FRAMEWORK_DYLIB=$(dirname "$(dirname "$(dirname "$(dirname "$FRAMEWORK_BIN")")")")/Python3
fi
echo "Python interpreter: $FRAMEWORK_BIN"
echo "Python dylib      : $FRAMEWORK_DYLIB"

# ---------- step 1: stop anything currently running -------------------------

echo ""
echo "[1/8] Stopping any running daemon ..."
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
pkill -f claude_usage_daemon.py 2>/dev/null || true

# ---------- step 2: build the C launcher ------------------------------------

echo "[2/8] Compiling C launcher ..."
LAUNCHER_C="$(mktemp -t clawd_launcher.c.XXXXXX).c"
LAUNCHER_BIN="$(mktemp -t clawd_launcher.bin.XXXXXX)"
cat > "$LAUNCHER_C" <<'CEOF'
// Clawdmeter Daemon launcher. Compiled binary so macOS treats this as a
// "real" Mach-O main executable and attributes the bundle's Info.plist
// (especially NSBluetoothAlwaysUsageDescription) to the BLE call.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <time.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    char self_path[PATH_MAX];
    uint32_t size = sizeof(self_path);
    if (_NSGetExecutablePath(self_path, &size) != 0) {
        fprintf(stderr, "_NSGetExecutablePath failed\n"); return 1;
    }
    char real_self[PATH_MAX];
    if (!realpath(self_path, real_self)) strcpy(real_self, self_path);
    char *macos_dir = dirname(real_self);

    char python_path[PATH_MAX], script_path[PATH_MAX];
    snprintf(python_path, sizeof(python_path), "%s/../Resources/.venv/bin/python3", macos_dir);
    snprintf(script_path, sizeof(script_path), "%s/../Resources/claude_usage_daemon.py", macos_dir);

    const char *home = getenv("HOME"); if (!home) home = "/tmp";
    char log_dir[PATH_MAX], stdout_path[PATH_MAX], stderr_path[PATH_MAX];
    snprintf(log_dir, sizeof(log_dir), "%s/Library/Logs/claude-usage-daemon", home);
    mkdir(log_dir, 0755);
    snprintf(stdout_path, sizeof(stdout_path), "%s/stdout.log", log_dir);
    snprintf(stderr_path, sizeof(stderr_path), "%s/stderr.log", log_dir);

    FILE *flog = fopen(stdout_path, "a");
    if (flog) {
        time_t now = time(NULL); struct tm *t = localtime(&now); char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
        fprintf(flog, "[%s] C-launcher start\n", ts);
        fprintf(flog, "  python = %s\n  script = %s\n", python_path, script_path);
        fclose(flog);
    }
    int o = open(stdout_path, O_WRONLY|O_APPEND|O_CREAT, 0644);
    int e = open(stderr_path, O_WRONLY|O_APPEND|O_CREAT, 0644);
    if (o >= 0) { dup2(o, 1); close(o); }
    if (e >= 0) { dup2(e, 2); close(e); }

    char *new_argv[] = { python_path, script_path, NULL };
    execv(python_path, new_argv);
    perror("execv");
    return 1;
}
CEOF
cc -arch x86_64 -arch arm64 -O2 -Wall -o "$LAUNCHER_BIN" "$LAUNCHER_C"
rm -f "$LAUNCHER_C"

# ---------- step 3: wipe and recreate the bundle skeleton -------------------

echo "[3/8] Re-creating bundle skeleton at $BUNDLE ..."
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS"
mkdir -p "$BUNDLE/Contents/Resources"

# Info.plist — NSBluetoothAlwaysUsageDescription is the magic key; without
# it macOS silently denies BLE access. LSUIElement = true keeps the app off
# the Dock so it runs purely in the background.
cat > "$BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>ClawdmeterDaemon</string>
    <key>CFBundleIdentifier</key>
    <string>$LABEL</string>
    <key>CFBundleName</key>
    <string>Clawdmeter Daemon</string>
    <key>CFBundleDisplayName</key>
    <string>Clawdmeter Daemon</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSUIElement</key>
    <true/>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSBluetoothAlwaysUsageDescription</key>
    <string>Clawdmeter Daemon pushes your Claude Code usage to the e-paper dashboard over Bluetooth Low Energy.</string>
    <key>NSBluetoothPeripheralUsageDescription</key>
    <string>Clawdmeter Daemon pushes your Claude Code usage to the e-paper dashboard over Bluetooth Low Energy.</string>
</dict>
</plist>
EOF

cp "$LAUNCHER_BIN" "$BUNDLE/Contents/MacOS/ClawdmeterDaemon"
chmod +x "$BUNDLE/Contents/MacOS/ClawdmeterDaemon"
rm -f "$LAUNCHER_BIN"

# ---------- step 4: build the embedded venv ---------------------------------

echo "[4/8] Creating embedded venv (Apple Python 3.9) ..."
"$SYSTEM_PYTHON" -m venv "$BUNDLE/Contents/Resources/.venv"

# The venv contains symlinked python3 → /Library/.../python3. We need a real
# binary inside the bundle so TCC sees the path inside our .app. Replace the
# symlink with a copy of the real interpreter, then patch its dyld reference
# to point at the framework dylib by absolute path (otherwise dyld looks for
# @executable_path/../Python3 relative to the venv).
VENV_PY="$BUNDLE/Contents/Resources/.venv/bin/python3"
rm "$VENV_PY"
cp "$FRAMEWORK_BIN" "$VENV_PY"
chmod +x "$VENV_PY"
install_name_tool -change @executable_path/../../../../Python3 "$FRAMEWORK_DYLIB" "$VENV_PY" 2>/dev/null || true
# Also handle the alternate name some Apple builds use
install_name_tool -change @executable_path/../Python3 "$FRAMEWORK_DYLIB" "$VENV_PY" 2>/dev/null || true
codesign --force --sign - "$VENV_PY"

# Sanity check
if ! "$VENV_PY" --version >/dev/null 2>&1; then
    echo "Patched Python failed to run — install_name_tool patch needs adjusting."
    echo "Actual dyld references:"
    otool -L "$VENV_PY"
    exit 1
fi

# ---------- step 5: install Python deps -------------------------------------

echo "[5/8] Installing bleak + httpx ..."
"$VENV_PY" -m ensurepip --upgrade >/dev/null
"$VENV_PY" -m pip install --upgrade pip --quiet
"$VENV_PY" -m pip install -r "$DAEMON_SRC_DIR/requirements.txt" --quiet

# Patch bleak's 1-second state-update wait. The first time the user sees a
# Bluetooth prompt they need more than 1 s to actually click "Allow"; 120 s
# is a generous upper bound and only matters on first launch.
DELEGATE="$BUNDLE/Contents/Resources/.venv/lib/python3.9/site-packages/bleak/backends/corebluetooth/CentralManagerDelegate.py"
if [[ -f "$DELEGATE" ]]; then
    sed -i.bak 's/self\._did_update_state_event\.wait(1)/self._did_update_state_event.wait(120)/' "$DELEGATE"
    rm -f "$DELEGATE.bak"
fi

# ---------- step 6: copy the daemon script + sign --------------------------

echo "[6/8] Copying daemon script ..."
cp "$DAEMON_SRC_DIR/claude_usage_daemon.py" "$BUNDLE/Contents/Resources/claude_usage_daemon.py"

echo "[7/8] Codesigning the bundle (ad-hoc) ..."
codesign --deep --force --sign - "$BUNDLE"

# Register with LaunchServices so `open` picks the right bundle ID.
/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister -f "$BUNDLE" 2>/dev/null || true

# ---------- step 7: install the LaunchAgent --------------------------------

echo "[8/8] Installing LaunchAgent ..."
mkdir -p "$LOG_DIR" "$HOME/Library/LaunchAgents"

cat > "$LAUNCH_AGENT" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/bin/open</string>
        <string>-g</string>
        <string>$BUNDLE</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <false/>
    <key>ProcessType</key>
    <string>Background</string>
</dict>
</plist>
EOF

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$LAUNCH_AGENT"

cat <<EOF

=== Installed. ===

Bundle:     $BUNDLE
LaunchAgent:$LAUNCH_AGENT
Logs:       $LOG_DIR/stdout.log
            $LOG_DIR/stderr.log

>>> First-time setup <<<

macOS only prompts for Bluetooth permission the first time the bundle
actually requests it. To get the prompt now:

    open "$BUNDLE"

Click **Allow** when "ClawdmeterDaemon" wants to use Bluetooth.

After that, every login the LaunchAgent will start it in the background
(no Dock icon, no menu bar).

>>> Useful commands <<<

    tail -f $LOG_DIR/stdout.log
    launchctl kickstart -k "gui/\$(id -u)/$LABEL"     # restart
    launchctl bootout    "gui/\$(id -u)/$LABEL"       # stop + remove
EOF
