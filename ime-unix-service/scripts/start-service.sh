#!/usr/bin/env bash
set -euo pipefail

BIN=${LLAVON_IME_SERVICE_BIN:-/usr/bin/llavon-ime-unix-service}
SOCKET=${LLAVON_IME_SOCKET:-/run/user/$(id -u)/llavon-ime/ime.sock}
MODEL=${LLAVON_IME_MODEL:-/usr/share/llavon-ime/models/llavon-ime-llama-250m-Q4_K_M.gguf}
TABLES=${LLAVON_IME_TABLES:-/home/billy/coding/ime-fcitx5/ime-unix-service/ime-core/table}
LOG=${LLAVON_IME_LOG:-$HOME/.local/state/llavon-ime/ime-unix-service.log}

mkdir -p "$(dirname "$SOCKET")" "$(dirname "$LOG")"

exec "$BIN" \
  --socket "$SOCKET" \
  --model "$MODEL" \
  --tables "$TABLES" \
  --context-length "${LLAVON_IME_CONTEXT_LENGTH:-512}" \
  --threads "${LLAVON_IME_THREADS:-32}" \
  --gpu-layers "${LLAVON_IME_GPU_LAYERS:-999}" \
  --max-sessions "${LLAVON_IME_MAX_SESSIONS:-8}" \
  --max-idle-sessions "${LLAVON_IME_MAX_IDLE_SESSIONS:-4}" \
  --max-concurrent-predictions "${LLAVON_IME_MAX_CONCURRENT_PREDICTIONS:-2}" \
  --idle-timeout "${LLAVON_IME_IDLE_TIMEOUT:-1800}" \
  >>"$LOG" 2>&1
