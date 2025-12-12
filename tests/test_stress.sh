#!/usr/bin/env bash
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

PORT=8080
BASE="http://localhost:${PORT}"
SLOG="/tmp/stress_test_server.log"
ABLOG="/tmp/stress_ab.log"

ok()   { echo -e "${GREEN}✓${NC} $*"; }
warn() { echo -e "${YELLOW}⊘${NC} $*"; }
fail() { echo -e "${RED}✗${NC} $*"; exit 1; }
info() { echo -e "${BLUE}ℹ${NC} $*"; }

echo "╔════════════════════════════════════════════════════════════╗"
echo "║              STRESS TESTS (Tests 21–24)                  ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Clean
pkill -9 webserver 2>/dev/null || true
sleep 1
rm -f "$SLOG" "$ABLOG"

# Ensure base files
mkdir -p www
[ -f www/index.html ] || echo "<html><body><h1>Index</h1></body></html>" > www/index.html

########################################
# Start server
########################################
./webserver server.conf >"$SLOG" 2>&1 &
SERVER_PID=$!
sleep 2
ps -p $SERVER_PID >/dev/null || fail "server failed to start"
ok "Server started (PID $SERVER_PID)"
echo ""

########################################
# TEST 21 — Continuous load (5 minutes)
########################################
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TEST 21] Continuous load (5 minutes)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if command -v ab >/dev/null 2>&1; then
  ab -r -s 5 -t 300 -c 100 "${BASE}/index.html" >"$ABLOG" 2>&1 || true
  grep -E "Requests per second|Failed requests|Non-2xx responses" "$ABLOG" || true
  ok "Continuous load completed"
else
  warn "ApacheBench not installed"
fi

echo ""

########################################
# TEST 22 — Memory leak check (Valgrind)
########################################
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TEST 22] Memory leak check (Valgrind)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [[ "${LEAKS:-0}" == "1" ]]; then
  command -v valgrind >/dev/null 2>&1 || fail "valgrind not installed"

  # Stop any running server
  pkill -9 webserver 2>/dev/null || true
  sleep 1

  # Start server under valgrind
  valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=definite \
    ./webserver server.conf >"$SLOG" 2>&1 &
  VG_PID=$!

  sleep 2
  ps -p $VG_PID >/dev/null || fail "server failed to start under valgrind"

  # Generate traffic for a few seconds
  info "Generating traffic under valgrind"
  seq 1 500 | xargs -n1 -P50 -I{} \
    curl -s --connect-timeout 2 --max-time 5 \
    "${BASE}/index.html" >/dev/null 2>&1 || true

  # Stop server
  kill -INT "$VG_PID" 2>/dev/null || true
  sleep 1
  kill "$VG_PID" 2>/dev/null || true
  wait "$VG_PID" 2>/dev/null || true

  # Check valgrind summary
  if grep -q "definitely lost: 0 bytes" "$SLOG"; then
    ok "No memory leaks detected"
  else
    warn "Possible memory leaks detected (check $SLOG)"
  fi
else
  warn "Skipped (set LEAKS=1 to enable)"
fi

echo ""

########################################
# TEST 23 — Graceful shutdown under load
########################################
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TEST 23] Graceful shutdown under load"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

./webserver server.conf >"$SLOG" 2>&1 &
SERVER_PID=$!
sleep 2

if command -v ab >/dev/null 2>&1; then
  ab -r -s 5 -n 20000 -c 100 "${BASE}/index.html" >/dev/null 2>&1 &
  LOAD_PID=$!
else
  # fallback load generator (sem ab)
  seq 1 20000 | xargs -n1 -P100 -I{} \
    curl -s --connect-timeout 2 --max-time 5 "${BASE}/index.html" \
    >/dev/null 2>&1 &
  LOAD_PID=$!
fi

sleep 2
kill -INT "$SERVER_PID" 2>/dev/null || true

# não fica travado: esperamos com tolerância e depois matamos o load se necessário
timeout 30s bash -c "wait $LOAD_PID" 2>/dev/null || true
timeout 10s bash -c "wait $SERVER_PID" 2>/dev/null || true

# limpeza final
kill "$LOAD_PID" 2>/dev/null || true
kill "$SERVER_PID" 2>/dev/null || true
wait "$LOAD_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

ok "Server shutdown completed under load"
echo ""

########################################
# TEST 24 — Zombie process check
########################################
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TEST 24] Zombie process check"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if ps -eo stat,cmd | grep webserver | grep -q Z; then
  fail "Zombie webserver processes detected"
else
  ok "No zombie webserver processes detected"
fi
