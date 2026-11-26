#!/bin/sh
# Drive the real TUI inside GNU screen. tmux and screen intercept different
# keys and render the alternative screen differently, so a tmux-only smoke does
# not cover the cell a screen user has. The command is a slash command, not a
# model turn: this checks the terminal path, not inference.
set -eu

bin="${1:-./amber}"
case "$bin" in
    /*) exe="$bin" ;;
    *) exe="$(pwd)/$bin" ;;
esac
session="amber-screen-$$"
root="$(mktemp -d "${TMPDIR:-/tmp}/amber-screen-XXXXXX")"
export SCREENDIR="$root/socket"
mkdir -p "$SCREENDIR" "$root/config" "$root/state" "$root/cache"
chmod 700 "$SCREENDIR"
cleanup() {
    screen -S "$session" -X quit >/dev/null 2>&1 || true
    rm -rf "$root"
}
trap cleanup EXIT INT TERM

command -v screen >/dev/null 2>&1 || {
    echo "screen-smoke: FAILED - screen is not installed" >&2
    exit 2
}

screen -dmS "$session" env \
    XDG_CONFIG_HOME="$root/config" XDG_STATE_HOME="$root/state" \
    XDG_CACHE_HOME="$root/cache" LANG=C.UTF-8 LC_ALL=C.UTF-8 "$exe"

capture() {
    rm -f "$root/cap"
    screen -S "$session" -X hardcopy "$root/cap" >/dev/null 2>&1 || true
    [ -f "$root/cap" ] && cat "$root/cap"
}

painted=0
for _ in $(seq 1 100); do
    if capture | grep -q "loaded completions"; then
        painted=1
        break
    fi
    sleep 0.2
done
if [ "$painted" -ne 1 ]; then
    echo "screen-smoke: FAILED - the UI never painted" >&2
    capture >&2
    exit 1
fi

screen -S "$session" -X stuff "/window list$(printf '\r')"
listed=0
for _ in $(seq 1 50); do
    if capture | grep -q "windows:"; then
        listed=1
        break
    fi
    sleep 0.2
done
if [ "$listed" -ne 1 ]; then
    echo "screen-smoke: FAILED - a typed command was not shown" >&2
    capture >&2
    exit 1
fi

echo "screen-smoke: passed"
