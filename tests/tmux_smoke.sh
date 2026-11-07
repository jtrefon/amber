#!/bin/sh
# Drive the real TUI inside a real tmux pane. The pty suite uses a bare pty;
# tmux is the screen-256color terminal a user actually has, and it is where the
# welcome-window/slash-command bug was visible.
set -eu

bin="${1:-./amber}"
case "$bin" in
    /*) exe="$bin" ;;
    *) exe="$(pwd)/$bin" ;;
esac
session="amber-tmux-$$"
root="$(mktemp -d "${TMPDIR:-/tmp}/amber-tmux-XXXXXX")"
cleanup() {
    tmux kill-session -t "$session" >/dev/null 2>&1 || true
    rm -rf "$root"
}
trap cleanup EXIT INT TERM

command -v tmux >/dev/null 2>&1 || {
    echo "tmux-smoke: FAILED - tmux is not installed" >&2
    exit 2
}

mkdir -p "$root/config" "$root/state" "$root/cache"
tmux new-session -d -s "$session" -x 100 -y 30 \
    "env XDG_CONFIG_HOME=$root/config XDG_STATE_HOME=$root/state XDG_CACHE_HOME=$root/cache $exe"

painted=0
for _ in $(seq 1 100); do
    if tmux capture-pane -p -t "$session" 2>/dev/null | grep -q "❯"; then
        painted=1
        break
    fi
    sleep 0.2
done
if [ "$painted" -ne 1 ]; then
    echo "tmux-smoke: FAILED - the UI never painted" >&2
    tmux capture-pane -p -t "$session" >&2 2>/dev/null || true
    exit 1
fi

tmux send-keys -t "$session" -l "/window list"
sleep 0.3
tmux send-keys -t "$session" Enter
listed=0
for _ in $(seq 1 50); do
    if tmux capture-pane -p -t "$session" 2>/dev/null | grep -q "windows:"; then
        listed=1
        break
    fi
    sleep 0.2
done
if [ "$listed" -ne 1 ]; then
    echo "tmux-smoke: FAILED - a typed command was not shown" >&2
    tmux capture-pane -p -t "$session" >&2 2>/dev/null || true
    exit 1
fi

echo "tmux-smoke: passed"
