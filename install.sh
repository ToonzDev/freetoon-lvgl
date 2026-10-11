#!/bin/sh
# freetoon-lvgl installer & updater.
#
# Replaces the stock Eneco qt-gui with the LVGL UI, sets up the boot picker
# so the user can always switch back, and turns on the VNC respawn so the
# Toon's screen is reachable remotely. Idempotent — re-run to upgrade.
#
# Usage:
#   1) Directly ON the Toon device:
#      sh ./install.sh                           # auto-detects model, uses local or downloads binaries
#      sh ./install.sh --download                # force download latest release from GitHub
#      sh ./install.sh --uninstall               # revert back to stock qt-gui
#
#   2) Remotely from a PC (via SSH):
#      TOON_HOST=192.168.x.x ./install.sh
#      ./install.sh --uninstall
#
# All optional integrations (HomeWizard P1 / WTR, Itho ventilation, Home
# Assistant) are OFF by default; flip them on later from Settings →
# Integrations on the Toon.

set -eu

TOON_HOST="${TOON_HOST:-toon}"
TOON_PASS="${TOON_PASS:-toon}"
TOON_USER="${TOON_USER:-root}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DL_DIR="/tmp/freetoon.$$"
FORCE_DOWNLOAD=0

# Detect if running ON the Toon directly (BusyBox, root, /etc/inittab, /mnt/data)
IS_LOCAL=0
if [ -f /etc/inittab ] && [ -d /mnt/data ] && { [ -f /etc/opkg/arch.conf ] || [ -d /qmf ]; }; then
    IS_LOCAL=1
fi

# Detect Toon model (Toon 1 vs Toon 2)
IS_TOON1=0
if [ -f /etc/opkg/arch.conf ] && ! grep -q nxt /etc/opkg/arch.conf 2>/dev/null; then
    IS_TOON1=1
fi

say() { echo "[freetoon] $*"; }

# Resolve each artefact — checks override var, then candidate paths
pick() {
    _val="$1"; shift
    if [ -n "$_val" ] && [ -e "$_val" ]; then echo "$_val"; return 0; fi
    for _c in "$@"; do
        if [ -n "$_c" ] && [ -e "$_c" ]; then echo "$_c"; return 0; fi
    done
    echo ""
}

resolve_artefacts() {
    if [ "$IS_TOON1" = 1 ]; then
        TOONUI_BIN="$(pick "${TOONUI_BIN:-}" "$DL_DIR/toonui" "$HERE/toonui-toon1" "$HERE/toonui" "$HERE/lvgl_ui_recovered/build1/toonui" "$HERE/lvgl_ui_recovered/build/toonui" "/mnt/data/toonui")"
    else
        TOONUI_BIN="$(pick "${TOONUI_BIN:-}" "$DL_DIR/toonui" "$HERE/toonui" "$HERE/lvgl_ui_recovered/build/toonui" "$HERE/lvgl_ui_recovered/build1/toonui" "$HERE/toonui-toon1" "/mnt/data/toonui")"
    fi
    UI_LAUNCHER="$(pick "${UI_LAUNCHER:-}"         "$DL_DIR/ui_launcher.sh"         "$HERE/ui_launcher.sh"         "$HERE/scripts/ui_launcher.sh"         "/mnt/data/ui_launcher.sh")"
    COMPANION_GATE="$(pick "${COMPANION_GATE:-}"   "$DL_DIR/companion_gate.sh"     "$HERE/companion_gate.sh"     "$HERE/scripts/companion_gate.sh"     "/mnt/data/companion_gate.sh")"
    INTEG_INSTALLER="$(pick "${INTEG_INSTALLER:-}" "$DL_DIR/integrations-install.sh" "$HERE/integrations-install.sh" "$HERE/scripts/integrations-install.sh" "/mnt/data/integrations-install.sh")"
    TOONVNC_SH="$(pick "${TOONVNC_SH:-}"           "$DL_DIR/toonvnc.sh"             "$HERE/toonvnc.sh"             "$HERE/scripts/toonvnc.sh"             "/mnt/data/toonvnc.sh")"
    OT_MODE_SH="$(pick "${OT_MODE_SH:-}"           "$DL_DIR/ot_mode_switch.sh"      "$HERE/ot_mode_switch.sh"      "$HERE/scripts/ot_mode_switch.sh"      "/mnt/data/ot_mode_switch.sh")"
    QUBY_BRIDGE_BIN="$(pick "${QUBY_BRIDGE_BIN:-}" "$DL_DIR/quby_bridge"            "$HERE/quby_bridge"            "$HERE/quby_bridge/quby_bridge"       "/mnt/data/quby_bridge")"
    FBVNC_INPUT="$(pick "${FBVNC_INPUT:-}"         "$DL_DIR/fbvnc_input"            "$HERE/fbvnc_input"            "$HERE/../qt_rebuild/fbvnc_input"     "/mnt/data/fbvnc_input")"

    PWA_DIR="${PWA_DIR:-}"
    if [ -z "$PWA_DIR" ]; then
        for d in "$DL_DIR/pwa/ui" "$HERE/pwa/ui" "$HERE/web/build" "$HERE/build" "/mnt/data/pwa/ui"; do
            if [ -f "$d/index.wasm" ]; then PWA_DIR="$d"; break; fi
        done
        : "${PWA_DIR:=$HERE/web/build}"
    fi
}

# Download matching release binaries & assets from GitHub (ToonzDev repository)
download_artefacts() {
    REPO="${REPO:-ToonzDev/freetoon-lvgl}"
    say "Resolving latest release from GitHub ($REPO)..."
    TAG=$(curl -fsSL --connect-timeout 8 --max-time 30 \
            "https://api.github.com/repos/$REPO/releases?per_page=1" 2>/dev/null \
          | grep -m1 '"tag_name"' | sed 's/.*"tag_name"[^"]*"\([^"]*\)".*/\1/')

    mkdir -p "$DL_DIR"
    RAW_BASE="https://raw.githubusercontent.com/$REPO/main"

    # Helper scripts directly from user repository
    for s in ui_launcher.sh companion_gate.sh ot_mode_switch.sh toonvnc.sh integrations-install.sh; do
        if [ ! -f "$HERE/scripts/$s" ] && [ ! -f "$HERE/$s" ] && [ ! -f "/mnt/data/$s" ]; then
            say "Fetching $s from $REPO..."
            curl -fSL --connect-timeout 8 --max-time 60 -o "$DL_DIR/$s" "$RAW_BASE/scripts/$s" 2>/dev/null \
                || curl -fSL --connect-timeout 8 --max-time 60 -o "$DL_DIR/$s" "$RAW_BASE/$s" 2>/dev/null || true
            [ -s "$DL_DIR/$s" ] && chmod +x "$DL_DIR/$s"
        fi
    done

    # Main UI binary from release if available
    if [ -n "$TAG" ]; then
        say "Latest release is $TAG (from $REPO)"
        BASE="https://github.com/$REPO/releases/download/$TAG"
        if [ "$IS_TOON1" = 1 ]; then
            BIN_ASSET="toonui-toon1"
            say "Detected Toon 1 (ARMv5) — fetching $BIN_ASSET..."
        else
            BIN_ASSET="toonui"
            say "Detected Toon 2 (ARMv7) — fetching $BIN_ASSET..."
        fi
        if curl -fSL --connect-timeout 8 --max-time 120 -o "$DL_DIR/toonui" "$BASE/$BIN_ASSET" 2>/dev/null; then
            chmod +x "$DL_DIR/toonui"
            TOONUI_BIN="$DL_DIR/toonui"
        else
            say "Note: could not download $BIN_ASSET from release $TAG on $REPO"
        fi

        # Optional VNC input bridge
        if [ ! -f "$HERE/fbvnc_input" ] && [ ! -f "/mnt/data/fbvnc_input" ]; then
            curl -fSL --connect-timeout 8 --max-time 60 -o "$DL_DIR/fbvnc_input" "$BASE/fbvnc_input" 2>/dev/null || true
            [ -s "$DL_DIR/fbvnc_input" ] && FBVNC_INPUT="$DL_DIR/fbvnc_input" && chmod +x "$FBVNC_INPUT"
        fi

        # Optional waste provider engine
        WF_ASSET="wastefetch"
        if [ "$IS_TOON1" = 1 ]; then WF_ASSET="wastefetch-toon1"; fi
        if [ ! -f "$HERE/$WF_ASSET" ] && [ ! -f "/mnt/data/wastefetch" ]; then
            curl -fSL --connect-timeout 8 --max-time 60 -o "$DL_DIR/wastefetch" "$BASE/$WF_ASSET" 2>/dev/null || true
            [ -s "$DL_DIR/wastefetch" ] && chmod +x "$DL_DIR/wastefetch"
        fi

        # WASM bundle for phone/browser UI
        mkdir -p "$DL_DIR/pwa/ui"
        for f in index.html index.wasm index.js; do
            curl -fSL --connect-timeout 8 --max-time 60 -o "$DL_DIR/pwa/ui/$f" "$BASE/$f" 2>/dev/null || true
        done
        [ -s "$DL_DIR/pwa/ui/index.wasm" ] && PWA_DIR="$DL_DIR/pwa/ui"

        # Bundled x11vnc if needed
        if [ ! -x /usr/bin/x11vnc ] && ! which x11vnc >/dev/null 2>&1 && [ ! -x /mnt/data/x11vnc-bundle/bin/x11vnc ]; then
            say "Fetching x11vnc bundle..."
            if curl -fSL --connect-timeout 8 --max-time 120 -o "$DL_DIR/x11vnc-bundle.tgz" "$BASE/x11vnc-bundle.tgz" 2>/dev/null; then
                mkdir -p /mnt/data/x11vnc-bundle
                tar xzf "$DL_DIR/x11vnc-bundle.tgz" -C /mnt/data/x11vnc-bundle 2>/dev/null || true
                chmod +x /mnt/data/x11vnc-bundle/bin/x11vnc 2>/dev/null || true
            fi
        fi
    else
        say "Notice: no release tag found on https://github.com/$REPO/releases (using local/existing binaries)."
    fi

    return 0
}

if [ "$IS_LOCAL" = 1 ]; then
    remote() { sh -c "$*"; }
    push() {
        _src="$1"
        _dest="$2"
        [ -z "$_src" ] && return 0
        if [ "$_src" = "$_dest" ]; then
            chmod +x "$_dest" 2>/dev/null || true
            return 0
        fi
        if [ ! -e "$_src" ]; then
            if [ -e "$_dest" ]; then
                say "Notice: $_src not found, keeping existing $_dest"
                chmod +x "$_dest" 2>/dev/null || true
                return 0
            fi
            say "Warning: $_src not found and $_dest does not exist"
            return 1
        fi
        echo "  → $_dest"
        cp -f "$_src" "${_dest}.new"
        mv -f "${_dest}.new" "$_dest"
        chmod +x "$_dest"
    }
else
    SSH="sshpass -p $TOON_PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $TOON_USER@$TOON_HOST"
    SCP="sshpass -p $TOON_PASS scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
    remote() { $SSH "$@"; }
    push() {
        _src="$1"
        _dest="$2"
        [ -z "$_src" ] && return 0
        if [ ! -e "$_src" ]; then
            say "Notice: $_src not found locally, skipping remote copy to $_dest"
            return 0
        fi
        echo "  → $_dest"
        $SCP "$_src" "$TOON_USER@$TOON_HOST:${_dest}.new"
        remote "mv -f ${_dest}.new ${_dest} && chmod +x ${_dest}"
    }
fi

TOON_ROW="toon:345:respawn:/mnt/data/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1"
VNCS_ROW="vncs:345:respawn:/mnt/data/toonvnc.sh respawn >> /var/volatile/tmp/x11vnc.log 2>&1"

require() {
    if [ "$IS_LOCAL" = 1 ]; then return 0; fi
    command -v "$1" >/dev/null 2>&1 || { echo "ERROR: '$1' not on PATH — apt-get install sshpass" >&2; exit 2; }
}

upsert_row() {
    _row="$1"
    _id="${1%%:*}"
    remote "grep -v '^${_id}:' /etc/inittab > /etc/inittab.new && echo '${_row}' >> /etc/inittab.new && mv -f /etc/inittab.new /etc/inittab"
}

drop_row() {
    remote "grep -v '^${1}:' /etc/inittab > /etc/inittab.new && mv -f /etc/inittab.new /etc/inittab"
}

disable_stock_qt() {
    # Comment out any stock Qt inittab rows (qtqt:, gui:, etc. running startqt or qt-gui)
    # so they do not fight FreeToon for the framebuffer.
    remote "sed -i -E 's~^([^#].*(startqt|qt-gui).*)~#FREETOON#\1~' /etc/inittab 2>/dev/null || true"
}

restore_stock_qt() {
    # Restore stock Qt inittab rows upon uninstalling FreeToon
    remote "sed -i 's~^#FREETOON#~~' /etc/inittab 2>/dev/null || true"
    remote "if ! grep -qE '^[^#].*(startqt|qt-gui)' /etc/inittab 2>/dev/null; then sed -i -E 's~^#+[ \t]*(qtqt:.*)~\1~' /etc/inittab 2>/dev/null || true; fi"
    remote "if ! grep -qE '^[^#].*(startqt|qt-gui)' /etc/inittab 2>/dev/null; then echo 'qtqt:245:respawn:/usr/bin/startqt >/var/log/qt 2>&1' >> /etc/inittab; fi"
}

check_artefacts() {
    resolve_artefacts

    # If toonui binary is missing or forced download requested: fetch from GitHub
    if [ -z "$TOONUI_BIN" ] || [ ! -f "$TOONUI_BIN" ] || [ "$FORCE_DOWNLOAD" = 1 ]; then
        say "No pre-compiled toonui binary found locally (or --download was specified)."
        download_artefacts || true
        resolve_artefacts
    fi

    missing=0
    for pair in "toonui:$TOONUI_BIN" "ui_launcher.sh:$UI_LAUNCHER" \
                "companion_gate.sh:$COMPANION_GATE" "toonvnc.sh:$TOONVNC_SH" \
                "ot_mode_switch.sh:$OT_MODE_SH"; do
        name="${pair%%:*}"
        path="${pair#*:}"
        if [ -z "$path" ] || [ ! -e "$path" ]; then
            echo "  missing $name: ${path:-not found}" >&2
            missing=1
        fi
    done

    if [ "$missing" -ne 0 ]; then
        say "Some required scripts are missing. Attempting download from $REPO..."
        download_artefacts || true
        resolve_artefacts
    fi

    if [ -z "$TOONUI_BIN" ] || [ ! -f "$TOONUI_BIN" ]; then
        say "FATAL: toonui binary could not be resolved or downloaded."
        say "Please ensure a release exists on https://github.com/$REPO or place toonui at /mnt/data/toonui."
        exit 3
    fi
    [ -n "$FBVNC_INPUT" ] && [ -x "$FBVNC_INPUT" ] || echo "  note: fbvnc_input absent — VNC will be view-only (not fatal)" >&2
}

do_install() {
    echo "[1/5] Verifying artefacts..."
    check_artefacts

    target_desc="$TOON_HOST"
    if [ "$IS_LOCAL" = 1 ]; then target_desc="local Toon"; fi
    echo "[2/5] Installing binaries + scripts to $target_desc..."
    push "$TOONUI_BIN"      /mnt/data/toonui
    push "$UI_LAUNCHER"     /mnt/data/ui_launcher.sh
    push "$COMPANION_GATE"  /mnt/data/companion_gate.sh
    push "$INTEG_INSTALLER" /mnt/data/integrations-install.sh
    push "$TOONVNC_SH"      /mnt/data/toonvnc.sh
    push "$OT_MODE_SH"      /mnt/data/ot_mode_switch.sh
    [ -n "$FBVNC_INPUT" ] && [ -x "$FBVNC_INPUT" ] && push "$FBVNC_INPUT" /mnt/data/fbvnc_input
    if [ -f "$DL_DIR/wastefetch" ]; then
        push "$DL_DIR/wastefetch" /mnt/data/wastefetch
    elif [ -f "$HERE/wastefetch" ]; then
        push "$HERE/wastefetch" /mnt/data/wastefetch
    fi

    echo "[3/5] Setting up WASM slave UI bundle..."
    if [ -n "$PWA_DIR" ] && [ -d "$PWA_DIR" ] && [ -f "$PWA_DIR/index.wasm" ]; then
        remote "mkdir -p /mnt/data/pwa/ui"
        for f in index.html index.js index.wasm; do
            [ -f "$PWA_DIR/$f" ] || continue
            echo "  → /mnt/data/pwa/ui/$f"
            if [ "$IS_LOCAL" = 1 ]; then
                if [ "$PWA_DIR/$f" != "/mnt/data/pwa/ui/$f" ]; then
                    cp -f "$PWA_DIR/$f" "/mnt/data/pwa/ui/${f}.new"
                    mv -f "/mnt/data/pwa/ui/${f}.new" "/mnt/data/pwa/ui/$f"
                fi
            else
                $SCP "$PWA_DIR/$f" "$TOON_USER@$TOON_HOST:/mnt/data/pwa/ui/${f}.new"
                remote "mv -f /mnt/data/pwa/ui/${f}.new /mnt/data/pwa/ui/${f}"
            fi
        done
        remote '[ -s /mnt/data/pwa/ui/index.wasm ] && cd /mnt/data/pwa && rm -f \
            app.js sw.js manifest.json icon-192.png index.html index.js index.wasm \
            index.html.bak index.html.staticbak index.js.bak index.wasm.bak || true'
    else
        echo "  (skipping WASM bundle — not found or not built)"
    fi

    echo "[4/5] Seeding default config..."
    # Toon 1 default to qt-gui on first install; Toon 2 default to freetoon
    if [ "$IS_TOON1" = 1 ]; then
        remote "[ -f /mnt/data/ui_choice ] || echo qt-gui > /mnt/data/ui_choice"
    else
        remote "[ -f /mnt/data/ui_choice ] || echo freetoon > /mnt/data/ui_choice"
    fi
    # toonui.cfg — write a basic-mode default ONLY if user has no cfg yet.
    remote "[ -f /mnt/data/toonui.cfg ] || cat > /mnt/data/toonui.cfg" <<'CFG'
auto_dim_enabled=1
auto_dim_seconds=10
active_brightness=800
dim_brightness=80
boot_picker_enabled=1
enable_p1_elec=0
enable_p1_water=0
enable_vent=0
enable_ha=0
CFG

    echo "[5/5] Wiring inittab + firewall + restart..."
    # Open PWA (10081) and VNC (5900) ports in stock Toon firewall
    if [ "$IS_LOCAL" = 1 ]; then
        FW=/etc/default/iptables.conf
        for p in 10081 5900; do
            if [ -f "$FW" ] && ! grep -q "dport $p " "$FW"; then
                sed -i "/--dport 80 --tcp-flags/a -A HCB-INPUT -p tcp -m tcp --dport $p --tcp-flags SYN,RST,ACK SYN -j ACCEPT" "$FW" 2>/dev/null || true
            fi
            if [ -x /usr/sbin/iptables ]; then
                /usr/sbin/iptables -C HCB-INPUT -p tcp --dport "$p" -j ACCEPT 2>/dev/null \
                    || /usr/sbin/iptables -I HCB-INPUT 1 -p tcp --dport "$p" -j ACCEPT 2>/dev/null || true
            fi
        done
    fi

    drop_row qbri
    drop_row p1br
    disable_stock_qt
    remote "pkill -x quby_bridge 2>/dev/null; pkill -x p1bridge 2>/dev/null; true"
    remote "umount -l /dev/ttymxc0 2>/dev/null; true"
    upsert_row "$TOON_ROW"
    upsert_row "$VNCS_ROW"
    if [ -n "$QUBY_BRIDGE_BIN" ] && [ -x "$QUBY_BRIDGE_BIN" ]; then
        echo "  → /mnt/data/quby_bridge"
        if [ "$IS_LOCAL" = 1 ]; then
            if [ "$QUBY_BRIDGE_BIN" != "/mnt/data/quby_bridge" ]; then
                cp -f "$QUBY_BRIDGE_BIN" /mnt/data/quby_bridge.new
                mv -f /mnt/data/quby_bridge.new /mnt/data/quby_bridge && chmod +x /mnt/data/quby_bridge
            else
                chmod +x /mnt/data/quby_bridge 2>/dev/null || true
            fi
        else
            $SCP "$QUBY_BRIDGE_BIN" "$TOON_USER@$TOON_HOST:/mnt/data/quby_bridge.new"
            remote "mv -f /mnt/data/quby_bridge.new /mnt/data/quby_bridge && chmod +x /mnt/data/quby_bridge"
        fi
        remote "/mnt/data/ot_mode_switch.sh proxy" || true
    else
        echo "  (skipping bridge install — quby_bridge absent; OT bridge stays off)"
    fi
    remote "pkill -9 -x toonui 2>/dev/null; pkill -9 -x x11vnc 2>/dev/null; pkill -9 -x qt-gui 2>/dev/null; pkill -9 -f /qmf/sbin/qt-gui 2>/dev/null; pkill -9 -x startqt 2>/dev/null; true"
    remote "kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true"

    rm -rf "$DL_DIR" 2>/dev/null || true

    echo
    echo "Installed successfully! Toon is now running FreeToon."
    echo "  • Boot picker: 10 s window at boot to switch to stock qt-gui"
    echo "  • Settings → UI mode: switch between FreeToon and qt-gui"
    echo "  • Settings → Integrations: configure P1 / Itho / HA"
}

do_uninstall() {
    echo "[1/4] Dropping inittab rows..."
    drop_row toon
    drop_row vncs
    drop_row p1br
    drop_row qbri
    echo "[2/4] Restoring stock qt inittab row..."
    restore_stock_qt
    echo "[3/4] Stopping daemons..."
    remote "pkill -9 -x toonui 2>/dev/null; pkill -9 -x x11vnc 2>/dev/null; pkill -9 -x quby_bridge 2>/dev/null; pkill -9 -x p1bridge 2>/dev/null; true"
    echo "[4/4] Removing files..."
    remote "rm -f /mnt/data/toonui /mnt/data/ui_launcher.sh /mnt/data/companion_gate.sh /mnt/data/toonvnc.sh /mnt/data/ot_mode_switch.sh /mnt/data/fbvnc_input /mnt/data/ui_choice /mnt/data/toonvnc.plain /mnt/data/toonui.cfg"
    remote "rm -rf /mnt/data/pwa"
    remote "kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true"
    echo "Uninstalled. Stock qt-gui takes over on next launcher cycle."
}

case "${1:-install}" in
    install|"")
        [ "$IS_LOCAL" = 0 ] && { require sshpass; require ssh; require scp; }
        do_install
        ;;
    --download|--update)
        FORCE_DOWNLOAD=1
        [ "$IS_LOCAL" = 0 ] && { require sshpass; require ssh; require scp; }
        do_install
        ;;
    --uninstall)
        [ "$IS_LOCAL" = 0 ] && { require sshpass; require ssh; require scp; }
        do_uninstall
        ;;
    -h|--help)
        sed -n '2,20p' "$0"
        ;;
    *)
        echo "Unknown command '$1' — see --help" >&2
        exit 1
        ;;
esac
