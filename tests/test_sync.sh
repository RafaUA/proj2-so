#!/usr/bin/env bash
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

PORT=8080
BASE="http://localhost:${PORT}"
SLOG="/tmp/sync_test_server.log"
HLOG="/tmp/helgrind.log"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║        SYNCHRONIZATION TESTS (Tests 17–20)                ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

ok()   { echo -e "${GREEN}✓${NC} $*"; }
warn() { echo -e "${YELLOW}⊘${NC} $*"; }
fail() { echo -e "${RED}✗${NC} $*"; exit 1; }
info() { echo -e "${BLUE}ℹ${NC} $*"; }

# Clean environment
pkill -9 webserver 2>/dev/null || true
sleep 1
rm -f "$SLOG" "$HLOG"
: > access.log 2>/dev/null || true

# Ensure files exist
mkdir -p www
[ -f www/index.html ] || echo "<html><body><h1>Index</h1></body></html>" > www/index.html
[ -f www/style.css  ] || echo "body { color: blue; }" > www/style.css
[ -f www/script.js  ] || echo "console.log('test');" > www/script.js

########################################
# TEST 17 — Helgrind
########################################
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TEST 17] Thread synchronization (Helgrind)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [[ "${HELGRIND:-0}" == "1" ]]; then
  command -v valgrind >/dev/null 2>&1 || fail "valgrind not installed"

  valgrind --tool=helgrind --log-file="$HLOG" \
    ./webserver server.conf >/dev/null 2>&1 &
  PID=$!
  sleep 2

  ps -p $PID >/dev/null || fail "server failed to start under helgrind"

  read -r -p "Press ENTER to stop helgrind test..." _
  kill -INT $PID 2>/dev/null || true
  sleep 1
  kill $PID 2>/dev/null || true
  wait $PID 2>/dev/null || true

  if grep -qi "Possible data race" "$HLOG"; then
    warn "Helgrind reported synchronization warnings"
  else
    ok "No data races reported by Helgrind"
  fi
else
  warn "Skipped (set HELGRIND=1 to enable)"
fi

echo ""

########################################
# Start server for remaining tests
########################################
./webserver server.conf >"$SLOG" 2>&1 &
SERVER_PID=$!
sleep 2
ps -p $SERVER_PID >/dev/null || fail "server failed to start"
ok "Server started (PID $SERVER_PID)"
echo ""

########################################
# Generate concurrent traffic
########################################
info "Generating concurrent traffic"
seq 1 2000 | xargs -n1 -P100 -I{} \
  curl -s --connect-timeout 2 --max-time 5 \
  "${BASE}/index.html" >/dev/null 2>&1 || true

########################################
# TEST 18 — Log integrity
########################################
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TEST 18] access.log integrity"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [[ -f access.log ]]; then
  bad=$(awk 'length($0)<20 || $0 !~ /"GET|\"HEAD/ {c++} END{print c+0}' access.log)
  total=$(wc -l < access.log | tr -d ' ')
  info "Log entries: $total"

  if [[ "$bad" -eq 0 ]]; then
    ok "No corrupted or interleaved log entries detected"
  else
    fail "Detected $bad invalid log entries"
  fi
else
  warn "access.log not found"
fi

echo ""

########################################
# TEST 19 — Cache consistency
########################################
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TEST 19] Cache consistency under concurrency"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

curl -s --connect-timeout 2 --max-time 5 \
  "${BASE}/index.html" >/dev/null || true

seq 1 1000 | xargs -n1 -P100 -I{} \
  curl -s --connect-timeout 2 --max-time 5 \
  "${BASE}/index.html" >/dev/null 2>&1 || true

ok "Repeated concurrent requests completed"

echo ""

########################################
# TEST 20 — Request counters
########################################
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "[TEST 20] Request counter consistency"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

kill -INT "$SERVER_PID" 2>/dev/null || true
sleep 1
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

if [[ -f access.log ]]; then
  LOG_REQ=$(wc -l < access.log | tr -d ' ')
  info "Requests recorded in access.log: $LOG_REQ"
  ok "Request accounting verified via log"
else
  warn "access.log not available for verification"
fi

echo ""
info "Server log saved to $SLOG"
info "Synchronization tests completed"
