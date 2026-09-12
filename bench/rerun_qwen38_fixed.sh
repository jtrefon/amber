#!/bin/bash
# Clean amber-bench rerun for Qwen 3.8 — on the MTP-desync-fixed binary, no
# repeat penalty (matches qwopus baseline conditions). Runs MTP + plain variants.
set -u
API="http://127.0.0.1:8081"
LOG=/tmp/opencode/amber-qwen38-fixed.log
mkdir -p /tmp/opencode
echo "=== amber qwen38 post-fix rerun $(date -Is) ===" > "$LOG"

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

echo ">> loading qwen38-27b-mtp" | tee -a "$LOG"
load_model "qwen38-27b-mtp" || exit 1
echo ">> running qwen38-27b-mtp bench..." | tee -a "$LOG"
./amber-bench run --live --profile qwen38-27b-mtp \
  --out bench/results/qwen38-27b-mtp-v5.json >> "$LOG" 2>&1
./amber-bench scorecard bench/results/qwen38-27b-mtp-v5.json \
  > bench/results/qwen38-27b-mtp-v5-scorecard.txt 2>&1
echo ">> qwen38-27b-mtp done: $(grep 'model score' bench/results/qwen38-27b-mtp-v5-scorecard.txt 2>/dev/null)" | tee -a "$LOG"

echo ">> loading qwen38-27b (plain)" | tee -a "$LOG"
load_model "qwen38-27b" || exit 1
echo ">> running qwen38-27b bench..." | tee -a "$LOG"
./amber-bench run --live --profile qwen38-27b \
  --out bench/results/qwen38-27b-plain-v2.json >> "$LOG" 2>&1
./amber-bench scorecard bench/results/qwen38-27b-plain-v2.json \
  > bench/results/qwen38-27b-plain-v2-scorecard.txt 2>&1
echo ">> qwen38-27b plain done: $(grep 'model score' bench/results/qwen38-27b-plain-v2-scorecard.txt 2>/dev/null)" | tee -a "$LOG"

echo "=== done $(date -Is) ===" | tee -a "$LOG"
