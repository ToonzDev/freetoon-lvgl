#!/bin/sh
# switch_to_freetoon.sh — Install FreeToon if not present, or switch to FreeToon from stock qt-gui.
# Designed for Toon 1 and Toon 2.
# Can be run directly from shell or kicked off by TSC / a QML app.

set -e

# Prevent being killed if parent process (e.g. qt-gui / TSC child) exits
trap '' HUP TERM

LOG="/var/volatile/tmp/switch_to_freetoon.log"
mkdir -p /var/volatile/tmp 2>/dev/null || true

log() {
    _msg="[$(date '+%Y-%m-%d %H:%M:%S')] [switch_to_freetoon] $*"
    echo "$_msg"
    echo "$_msg" >> "$LOG" 2>/dev/null || true
}

CHOICE_FILE="/mnt/data/ui_choice"
TOONUI_BIN="/mnt/data/toonui"
UI_LAUNCHER="/mnt/data/ui_launcher.sh"
INITTAB="/etc/inittab"
REPO_INSTALL_URL="https://raw.githubusercontent.com/ToonzDev/freetoon-lvgl/main/install.sh"

log "========================================================"
log "Starting FreeToon switch/install helper..."

# 1. Hardware detection
IS_TOON1=0
if [ -f /etc/opkg/arch.conf ] && ! grep -q nxt /etc/opkg/arch.conf 2>/dev/null; then
    IS_TOON1=1
    log "Hardware detected: Toon 1"
else
    log "Hardware detected: Toon 2"
fi

# Reset crash counters so launcher gives FreeToon a clean start
rm -f /var/volatile/tmp/toonui_fails /var/volatile/tmp/toonui_restart 2>/dev/null || true

# 2. Check if FreeToon is already installed
IS_INSTALLED=0
if [ -f "$TOONUI_BIN" ] && [ -x "$TOONUI_BIN" ] && [ -f "$UI_LAUNCHER" ] && [ -x "$UI_LAUNCHER" ]; then
    IS_INSTALLED=1
fi

if [ "$IS_INSTALLED" = 0 ]; then
    log "FreeToon is NOT installed. Starting installation..."

    # Pre-set choice to freetoon so installer doesn't default to qt-gui on Toon 1
    echo "freetoon" > "$CHOICE_FILE"

    # Locate installer: local file or download
    INSTALLER=""
    HERE="$(cd "$(dirname "$0")" 2>/dev/null && pwd || echo "")"
    for cand in "$HERE/install.sh" "$HERE/../install.sh" /mnt/data/install.sh /root/install.sh; do
        if [ -f "$cand" ]; then
            INSTALLER="$cand"
            break
        fi
    done

    TMP_INSTALLER="/tmp/freetoon_install_$$.sh"
    if [ -z "$INSTALLER" ]; then
        log "Fetching latest install.sh from GitHub..."
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -4 --connect-timeout 10 --max-time 60 -o "$TMP_INSTALLER" "$REPO_INSTALL_URL" 2>/dev/null || \
            curl -k -fsSL --connect-timeout 10 --max-time 60 -o "$TMP_INSTALLER" "$REPO_INSTALL_URL" 2>/dev/null || true
        elif command -v wget >/dev/null 2>&1; then
            wget --no-check-certificate -q -O "$TMP_INSTALLER" "$REPO_INSTALL_URL" 2>/dev/null || true
        fi

        if [ -s "$TMP_INSTALLER" ]; then
            INSTALLER="$TMP_INSTALLER"
        fi
    fi

    if [ -z "$INSTALLER" ] || [ ! -s "$INSTALLER" ]; then
        log "ERROR: Could not download or locate install.sh. Check network connection."
        rm -f "$TMP_INSTALLER" 2>/dev/null || true
        exit 1
    fi

    log "Executing installer: $INSTALLER"
    chmod +x "$INSTALLER"
    if env FORCE_DOWNLOAD=1 sh "$INSTALLER" --download >> "$LOG" 2>&1; then
        log "Installation finished successfully."
    else
        log "ERROR: Installer failed. See details in $LOG."
        rm -f "$TMP_INSTALLER" 2>/dev/null || true
        exit 1
    fi
    rm -f "$TMP_INSTALLER" 2>/dev/null || true

    # Ensure selection remains freetoon
    echo "freetoon" > "$CHOICE_FILE"

else
    log "FreeToon is already installed. Switching active UI to FreeToon..."

    # Set UI preference to freetoon
    echo "freetoon" > "$CHOICE_FILE"

    # Ensure inittab has FreeToon launcher row and stock qt is disabled
    TOON_ROW="toon:345:respawn:/mnt/data/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1"
    if grep -qE '^([^#].*(startqt|qt-gui).*)' "$INITTAB" 2>/dev/null; then
        log "Disabling stock qt-gui in $INITTAB..."
        sed -i -E 's~^([^#].*(startqt|qt-gui).*)~#FREETOON#\1~' "$INITTAB" 2>/dev/null || true
    fi

    if ! grep -q '^toon:' "$INITTAB" 2>/dev/null; then
        log "Adding FreeToon launcher to $INITTAB..."
        echo "$TOON_ROW" >> "$INITTAB"
    fi

    # Switch OT bridge mode if proxy is enabled in toonui.cfg
    if [ -f /mnt/data/toonui.cfg ] && grep -q '^ot_bridge_mode=proxy' /mnt/data/toonui.cfg 2>/dev/null; then
        if [ -x /mnt/data/ot_mode_switch.sh ]; then
            log "Configuring OpenTherm proxy bridge..."
            /mnt/data/ot_mode_switch.sh proxy >> "$LOG" 2>&1 || true
        fi
    fi

    log "Signaling init to reload inittab..."
    kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
    sleep 1

    log "Stopping stock qt-gui processes..."
    killall -9 qt-gui 2>/dev/null || true
    pkill -9 -x qt-gui 2>/dev/null || true
    pkill -9 -f /qmf/sbin/qt-gui 2>/dev/null || true
    pkill -9 -x startqt 2>/dev/null || true

    # Final SIGHUP to make sure init immediately respawns ui_launcher.sh
    kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
fi

log "FreeToon launch initiated successfully."
exit 0

