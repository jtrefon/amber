#!/bin/bash
# Clean amber-bench rerun for Qwen 3.8 variants — post-loop-fix verification.
# Runs MTP and plain variants, then restores qwopus.
set -u
API="http://127.0.0.1:8081"
LOG=/tmp/opencode/amber-qwen38-rerun.log
mkdir -p /tmp/opencode
echo "=== amber qwen38 rerun $(date -Is) ===" > "$LOG"

wait_ready() {
  for i in $(seq 1 60); do
    sleep 5
    R=$(curl -s --max-time 5 "$API/v1/models" 2>/dev/null)
    if echo "$R" | grep -q '"models"' && ! echo "$R" | grep -q 'error'; then
      return 0
    fi
  done
  return 1
}

load_model() {
  local m="$1"
  curl -s --max-time 600 -X POST "$API/models/load" \
    -H 'Content-Type: application/json' \
    -d "{\"model\": \"$m\"}" >/dev/null 2>&1
  wait_ready
}

restore() {
  echo ">> restoring qwopus-27b" >> "$LOG"
  load_model "qwopus-27b" || echo "!! restore failed" >> "$LOG"
}
trap restore EXIT

cd /home/jack/Projects/cpp-agent || exit 1

# 1) Qwen 3.8 with MTP (same config as corrupted prior runs)
echo ">> loading qwen38-27b-mtp" | tee -a "$LOG"
load_model "qwen38-27b-mtp" || { echo "!! load failed" | tee -a "$LOG"; exit 1; }
echo ">> running qwen38-27b-mtp bench..." | tee -a "$LOG"
./amber-bench run --live --profile qwen38-27b-mtp \
  --out bench/results/qwen38-27b-mtp-v4.json >> "$LOG" 2>&1
./amber-bench scorecard bench/results/qwen38-27b-mtp-v4.json \
  > bench/results/qwen38-27b-mtp-v4-scorecard.txt 2>&1
echo ">> qwen38-27b-mtp done: $(grep 'model score' bench/results/qwen38-27b-mtp-v4-scorecard.txt 2>/dev/null)" | tee -a "$LOG"

# 2) Qwen 3.8 plain (no MTP — the verify-without-MTP run)
echo ">> loading qwen38-27b (plain)" | tee -a "$LOG"
load_model "qwen38-27b" || { echo "!! load failed" | tee -a "$LOG"; exit 1; }
echo ">> running qwen38-27b bench..." | tee -a "$LOG"
./amber-bench run --live --profile qwen38-27b \
  --out bench/results/qwen38-27b-plain-v1.json >> "$LOG" 2>&1
./amber-bench scorecard bench/results/qwen38-27b-plain-v1.json \
  > bench/results/qwen38-27b-plain-v1-scorecard.txt 2>&1
echo ">> qwen38-27b plain done: $(grep 'model score' bench/results/qwen38-27b-plain-v1-scorecard.txt 2>/dev/null)" | tee -a "$LOG"

echo "=== done $(date -Is) ===" | tee -a "$LOG"
