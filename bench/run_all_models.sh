#!/bin/bash
# Run the full model benchmark (49 scenarios) for every model registered in
# the llama-turboq in-process swap registry (models.ini aliases).
#
# Uses the service's documented in-process model swap (POST /models/load) —
# no restart, no config edit, no parallel server. The production model
# (qwopus) is restored at the end, unconditionally.
#
# Usage: bench/run_all_models.sh [--suite SUITE] [--dry-run]
set -u

MODELS="qwopus-27b qwen35-dense qwen35-pi-reasoning qwen35-moe qwen36-27b-mtp ornith-9b ornith-35b gemma4-31b gemma4-12b-q4 gemma4-12b-q8"
SUITE=""
DRY=0
for a in "$@"; do
  case "$a" in
    --suite) SUITE="$2"; shift 2 ;;
    --dry-run) DRY=1 ;;
  esac
done

API="http://127.0.0.1:8081"
LOG=/tmp/opencode/models-run.log
mkdir -p /tmp/opencode
echo "=== multi-model benchmark run $(date -Is) ===" > "$LOG"

wait_ready() {
  for i in $(seq 1 60); do
    sleep 5
    R=$(curl -s --max-time 5 "$API/v1/models" 2>/dev/null)
    if echo "$R" | grep -q '"models"'; then
      # still loading? the endpoint answers 503 "Loading model" via error
      if echo "$R" | grep -q 'error'; then continue; fi
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

for m in $MODELS; do
  if [ "$DRY" = 1 ]; then
    echo "dry-run: $m" | tee -a "$LOG"
    continue
  fi
  echo ">> [$m] loading..." | tee -a "$LOG"
  if ! load_model "$m"; then
    echo "!! [$m] load timeout — recording as failed" | tee -a "$LOG"
    continue
  fi
  echo ">> [$m] ready, running benchmark..." | tee -a "$LOG"
  OUT="bench/results/${m}-bench.json"
  if [ -n "$SUITE" ]; then
    ./amber-bench run --live --suite "$SUITE" --out "$OUT" >> "$LOG" 2>&1
  else
    ./amber-bench run --live --out "$OUT" >> "$LOG" 2>&1
  fi
  echo ">> [$m] done: $(tail -1 "$LOG")" | tee -a "$LOG"
  # per-model scorecard
  ./amber-bench scorecard "$OUT" > "bench/results/${m}-scorecard.txt" 2>&1
  echo ">> [$m] scorecard written" | tee -a "$LOG"
done

echo "=== done $(date -Is) ===" | tee -a "$LOG"
