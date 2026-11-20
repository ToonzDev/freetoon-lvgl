#!/bin/sh
# FreeToon diagnostics report, for support without SSH.
#
#   freetoon_diag.sh          print the report to stdout
#                             (toonui serves this live as :10081/diag)
#   freetoon_diag.sh --loop   refresh /var/volatile/tmp/freetoon-diag.txt every
#                             60 s; the stock web server serves it on port 80 as
#                             http://<toon>/freetoon-diag.txt. Started by
#                             ui_launcher.sh under BOTH UIs, so a user can make
#                             the same report under FreeToon and under the
#                             stock UI and we can compare them.
#
# Port 80 has no login, so the report holds nothing secret: process NAMES
# only (command lines can carry tokens), and only a fixed list of
# toonui.cfg keys. The report lives in RAM (tmpfs), never on flash.
#
# BusyBox-safe (Toon 1: no `command`, `head -n N` only, no pkill).

OUT=/var/volatile/tmp/freetoon-diag.txt
WEB=/qmf/www/freetoon-diag.txt
CFG=/mnt/data/toonui.cfg

section() { echo; echo "===== $* ====="; }

report() {
    echo "FreeToon diagnose - $(date '+%Y-%m-%d %H:%M:%S %Z')"
    if grep -q nxt /etc/opkg/arch.conf 2>/dev/null; then echo "model: Toon 2"; else echo "model: Toon 1"; fi
    echo "uptime: $(cat /proc/uptime 2>/dev/null | cut -d' ' -f1) s"

    section "UI"
    echo "ui_choice: $(cat /mnt/data/ui_choice 2>/dev/null | tr -d ' \r\n')"
    for p in toonui qt-gui; do
        pid=$(pidof "$p" 2>/dev/null)
        echo "$p: ${pid:-not running}"
    done
    if [ -f /mnt/data/toonui ]; then
        echo "toonui binary: $(wc -c < /mnt/data/toonui) bytes, md5 $(md5sum /mnt/data/toonui 2>/dev/null | cut -d' ' -f1)"
    fi

    section "toonui.cfg (selected keys)"
    for k in ot_bridge_mode otgw_host client_mode master_host energy_source boot_picker_enabled; do
        v=$(sed -n "s/^$k=//p" "$CFG" 2>/dev/null | head -n 1)
        echo "$k=$v"
    done

    section "Processes (name only, no kernel threads)"
    for d in /proc/[0-9]*; do
        # Kernel threads have an empty cmdline; skip them (bioset, kworker...).
        # Read it: /proc files always report size 0, so `[ -s ]` is useless.
        # Only tested for emptiness, never printed (it can hold tokens).
        c=$(tr -d '\000' < "$d/cmdline" 2>/dev/null)   # BusyBox head has no -c
        [ -n "$c" ] || continue
        name=$(cat "$d/comm" 2>/dev/null) || continue
        [ -n "$name" ] && echo "${d#/proc/} $name"
    done | sort -n

    section "Boiler serial line (/dev/ttymxc0)"
    found=0
    for fd in /proc/[0-9]*/fd/*; do
        t=$(readlink "$fd" 2>/dev/null) || continue
        case "$t" in *ttymxc0*)
            p=${fd#/proc/}; p=${p%%/*}
            echo "pid $p ($(cat /proc/$p/comm 2>/dev/null)) has $t open"
            found=1 ;;
        esac
    done
    [ "$found" = 1 ] || echo "nobody has /dev/ttymxc0 open"
    grep ttymxc /proc/mounts 2>/dev/null && echo "^ WARNING: something is mounted over the boiler UART" || echo "no mount over /dev/ttymxc0"

    section "inittab (active rows)"
    grep -v '^#' /etc/inittab 2>/dev/null | grep -v '^[[:space:]]*$'

    section "happ_thermstat getThermostatInfo"
    curl -s --max-time 4 'http://localhost:10080/happ_thermstat?action=getThermostatInfo' 2>/dev/null || echo "(no answer)"
    echo

    section "ot_mode_switch.log (last 20)"
    tail -n 20 /tmp/ot_mode_switch.log 2>/dev/null || echo "(none)"

    section "ui_launcher.log (last 20)"
    tail -n 20 /var/volatile/tmp/ui_launcher.log 2>/dev/null || echo "(none)"

    section "toonui.log boiler/thermostat lines (last 40)"
    grep -E '\[bxt\]|\[otgw\]|\[uimode\]|boiler|Boiler' /var/volatile/tmp/toonui.log 2>/dev/null | tail -n 40

    section "Memory / storage"
    free 2>/dev/null
    df -h / /var/volatile 2>/dev/null
}

if [ "${1:-}" = "--loop" ]; then
    sp=
    trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null; exit 0' TERM INT
    while :; do
        report > "$OUT.tmp" 2>&1 && mv -f "$OUT.tmp" "$OUT"
        # Point the stock web root at the RAM copy (once; a symlink, so the
        # 60 s refresh never writes flash).
        if [ -d /qmf/www ] && [ "$(readlink "$WEB" 2>/dev/null)" != "$OUT" ]; then
            ln -sf "$OUT" "$WEB" 2>/dev/null || true
        fi
        sleep 60 & sp=$!
        wait "$sp"
    done
fi

report
