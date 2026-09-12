#!/bin/bash
# Re-run qwen36-27b-mtp only — previous run scored 795.359 but JSON write
# failed with invalid UTF-8 byte (file was 0 bytes).
set -u
API="http://127.0.0.1:8081"
LOG=/tmp/opencode/qwen36-rerun.log
mkdir -p /tmp/opencode
echo "=== qwen36-27b-mtp rerun $(date -Is) ===" > "$LOG"

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

echo ">> loading qwen36-27b-mtp" | tee -a "$LOG"
load_model "qwen36-27b-mtp" || { echo "!! load failed" | tee -a "$LOG"; exit 1; }
echo ">> running qwen36-27b-mtp bench..." | tee -a "$LOG"
./amber-bench run --live --out bench/results/qwen36-27b-mtp-bench.json >> "$LOG" 2>&1
echo "run exit=$?" | tee -a "$LOG"
./amber-bench scorecard bench/results/qwen36-27b-mtp-bench.json \
  > bench/results/qwen36-27b-mtp-scorecard.txt 2>&1 || echo "scorecard FAILED" | tee -a "$LOG"
echo ">> done: $(grep -E 'model score' bench/results/qwen36-27b-mtp-scorecard.txt 2>/dev/null | head -1)" | tee -a "$LOG"
echo "=== done $(date -Is) ===" | tee -a "$LOG"
