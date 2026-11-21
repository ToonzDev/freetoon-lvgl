#!/bin/sh
# Boiler serial capture for support (BACKLOG #64): what happ_thermstat says to
# the boiler module over the UART, under FreeToon AND under the stock UI.
#
#   ketellog.sh start [minutes]  start a capture (default 60, 5..240 min)
#   ketellog.sh stop             stop it now
#   ketellog.sh status           one line: active / stopped, size, until
#   ketellog.sh expire           drop the hook once the time is up (diag loop)
#   ketellog.sh note             append one status line (diag loop, every min)
#
# How: the stock /etc/init.d/HCBv2 sources /tmp/etc-default-HCBv2 just before
# it execs an HCB app (XTRAARG = app name). A marked block there preloads
# /mnt/data/libttysniff.so into happ_thermstat only; then happ_thermstat is
# restarted so it picks it up. The tap only logs (never touches the line) and
# stops by itself at the deadline, at 20 MB or when the stop file exists.
# A UI switch restarts happ_thermstat (ot_mode_switch.sh) and the hook loads
# the tap again, so one capture covers both UIs; each start is labelled with
# the UI. Hook and log live in RAM: a reboot removes both.
#
# The log is served on port 80 as http://<toon>/freetoon-ketellog.txt (and
# via toonui :10081). It holds serial frames and thermostat values only.
#
# BusyBox-safe (no pkill, no `command`, head -n N only).

LIB=/mnt/data/libttysniff.so
HOOK=/tmp/etc-default-HCBv2
LOG=/var/volatile/tmp/ketellog.txt
STOPF=/var/volatile/tmp/ketellog.stop
WEB=/qmf/www/freetoon-ketellog.txt
BEGIN='# >>> freetoon ketellog (BACKLOG #64)'
END='# <<< freetoon ketellog'

now() { date +%s; }
stamp() { date '+%H:%M:%S.000'; }
slog() { [ -f "$LOG" ] && echo "$(stamp) S $*" >> "$LOG"; }

happ_pid() {
    for d in /proc/[0-9]*; do
        [ "$(cat "$d/comm" 2>/dev/null)" = happ_thermstat ] && { echo "${d#/proc/}"; return 0; }
    done
    return 1
}

hook_until() {
    [ -f "$HOOK" ] || return 1
    sed -n 's/^.*TTYSNIFF_UNTIL=\([0-9][0-9]*\).*$/\1/p' "$HOOK" | head -n 1
}

# Remove our block, keep anything else that may be in the hook file.
hook_drop() {
    [ -f "$HOOK" ] || return 0
    grep -qF "$BEGIN" "$HOOK" || return 0
    sed "/^# >>> freetoon ketellog/,/^# <<< freetoon ketellog/d" "$HOOK" > "$HOOK.new" 2>/dev/null || return 1
    if [ -s "$HOOK.new" ]; then mv -f "$HOOK.new" "$HOOK"; else rm -f "$HOOK.new" "$HOOK"; fi
}

restart_happ() {
    killall -9 happ_thermstat 2>/dev/null || {
        p=$(happ_pid) && kill -9 "$p" 2>/dev/null
    }
    true
}

thermostat_line() {
    j=$(curl -s --max-time 4 'http://localhost:10080/happ_thermstat?action=getThermostatInfo' 2>/dev/null)
    [ -n "$j" ] || { echo "thermostat: no answer"; return; }
    f() { echo "$j" | sed -n "s/.*\"$1\":\"\{0,1\}\([^\",}]*\).*/\1/p" | head -n 1; }
    echo "ui=$(cat /mnt/data/ui_choice 2>/dev/null | tr -d ' \r\n') temp=$(f currentTemp) setpoint=$(f currentSetpoint) burnerInfo=$(f burnerInfo) boilerSetpoint=$(f currentInternalBoilerSetpoint) programState=$(f programState) otCommError=$(f otCommError) happ=$(happ_pid)"
}

status() {
    u=$(hook_until)
    sz=0; [ -f "$LOG" ] && sz=$(wc -c < "$LOG")
    loaded=no
    p=$(happ_pid) && grep -q libttysniff "/proc/$p/maps" 2>/dev/null && loaded=yes
    if [ -n "$u" ] && [ "$(now)" -lt "$u" ] && [ ! -f "$STOPF" ]; then
        echo "active until $(date -d "@$u" '+%H:%M' 2>/dev/null || echo "$u") (still $(( (u - $(now)) / 60 )) min), log ${sz} bytes, tap in happ_thermstat: $loaded"
    elif [ "$sz" -gt 0 ]; then
        echo "stopped, log ${sz} bytes (http://<toon>/freetoon-ketellog.txt)"
    else
        echo "off"
    fi
}

case "${1:-status}" in
start)
    if [ ! -f "$LIB" ]; then
        echo "ERROR: $LIB missing (not in this release for this Toon model)"; exit 2
    fi
    if [ ! -d /tmp/ ]; then
        echo "ERROR: /tmp is not a directory (broken symlink?), HCBv2 cannot read the hook"; exit 3
    fi
    min=${2:-60}
    case "$min" in ''|*[!0-9]*) min=60 ;; esac
    [ "$min" -lt 5 ] && min=5
    [ "$min" -gt 240 ] && min=240
    until=$(( $(now) + min * 60 ))

    hook_drop
    rm -f "$STOPF" "$LOG.old"
    [ -f "$LOG" ] && mv -f "$LOG" "$LOG.old"
    {
        echo "FreeToon ketel-log (serial capture, BACKLOG #64)"
        echo "started $(date '+%Y-%m-%d %H:%M:%S %Z'), $min min"
        if grep -q nxt /etc/opkg/arch.conf 2>/dev/null; then echo "model: Toon 2"; else echo "model: Toon 1"; fi
        echo "W = happ_thermstat -> boiler module, R = module -> happ, T = line settings, S = status (every minute)"
    } > "$LOG"
    {
        echo "$BEGIN"
        echo "if [ \"\$XTRAARG\" = happ_thermstat ] && [ -f $LIB ] && [ \"\$(date +%s)\" -lt $until ] && [ ! -f $STOPF ]; then"
        echo "    export LD_PRELOAD=$LIB TTYSNIFF_UNTIL=$until TTYSNIFF_LOG=$LOG TTYSNIFF_STOP=$STOPF"
        echo "fi"
        echo "$END"
    } >> "$HOOK"
    if [ -d /qmf/www ] && [ "$(readlink "$WEB" 2>/dev/null)" != "$LOG" ]; then
        ln -sf "$LOG" "$WEB" 2>/dev/null || true
    fi
    slog "start $min min; $(thermostat_line)"
    restart_happ
    # Wait for init to respawn happ_thermstat (HCBv2 sleeps 1 s first).
    i=0; loaded=no
    while [ $i -lt 20 ]; do
        sleep 1; i=$((i + 1))
        p=$(happ_pid) || continue
        grep -q libttysniff "/proc/$p/maps" 2>/dev/null && { loaded=yes; break; }
    done
    slog "happ_thermstat restarted (pid ${p:-none}), tap loaded: $loaded"
    if [ "$loaded" != yes ]; then
        echo "ERROR: happ_thermstat did not load the tap (pid ${p:-none})"; exit 4
    fi
    # Safety: heating must never depend on the tap. If happ_thermstat with
    # the tap does not answer (or dies) within ~30 s, drop the hook and
    # restart it clean.
    i=0; ok=no
    while [ $i -lt 30 ]; do
        sleep 1; i=$((i + 1))
        q=$(happ_pid) || continue
        [ "$q" = "$p" ] || break      # it restarted: not healthy
        curl -s --max-time 3 'http://localhost:10080/happ_thermstat?action=getThermostatInfo' 2>/dev/null \
            | grep -q '"result":"ok"' && { ok=yes; break; }
    done
    if [ "$ok" != yes ]; then
        touch "$STOPF"
        hook_drop
        restart_happ
        slog "happ_thermstat not healthy with the tap: hook removed, restarted without it"
        echo "ERROR: happ_thermstat did not answer with the tap; capture cancelled"; exit 5
    fi
    echo "OK: capture running for $min min"
    ;;
stop)
    touch "$STOPF"
    hook_drop
    slog "stop requested; $(thermostat_line)"
    echo "stopped"
    ;;
expire)
    u=$(hook_until)
    if [ -n "$u" ] && { [ "$(now)" -ge "$u" ] || [ -f "$STOPF" ]; }; then
        hook_drop
        slog "capture period over, hook removed"
    fi
    ;;
note)
    u=$(hook_until)
    [ -n "$u" ] && [ "$(now)" -lt "$u" ] && [ ! -f "$STOPF" ] && slog "$(thermostat_line)"
    ;;
status)
    status
    ;;
*)
    echo "usage: $0 start [minutes] | stop | status" >&2
    exit 1
    ;;
esac
