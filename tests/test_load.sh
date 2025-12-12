#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# start server in foreground (no daemon) to control lifecycle here
./webserver -c server.conf > /tmp/webserver_test.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# give server a moment to start
sleep 1

echo "[test_load] Basic GET /index.html"
curl -s http://localhost:8080/index.html > /dev/null

echo "[test_load] HEAD /index.html"
curl -s -I http://localhost:8080/index.html > /dev/null

echo "[test_load] 404 check"
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/nonexistent.html

echo "[test_load] Concurrent curl (50 requests)"
for i in {1..50}; do
    curl -s --max-time 5 http://localhost:8080/index.html > /dev/null &
done
wait

echo "[test_load] Running test_concurrent binary"
if [[ -x tests/test_concurrent ]]; then
    tests/test_concurrent
fi

echo "[test_load] ApacheBench quick check (100 requests, 10 concurrency) if available"
if command -v ab >/dev/null 2>&1; then
    ab -n 100 -c 10 http://localhost:8080/index.html > /dev/null
else
    echo "[test_load] ab not installed; skipping ab test"
fi

if command -v ab >/dev/null 2>&1; then
    echo "[test_load] ab 1000 requests, 10 concurrency"
    ab -n 1000 -c 10 http://localhost:8080/index.html > /dev/null
    echo "[test_load] ab 10000 requests, 100 concurrency"
    ab -n 10000 -c 100 http://localhost:8080/index.html > /dev/null
    for f in index.html style.css script.js; do
        echo "[test_load] ab 1000 requests, 50 concurrency for $f"
        ab -n 1000 -c 50 http://localhost:8080/$f > /dev/null
    done
fi

echo "[test_load] Cache hit timing check (curl)"
if command -v time >/dev/null 2>&1; then
    echo "  first request (miss expected)"
    /usr/bin/time -f "  elapsed: %e s" curl -s http://localhost:8080/index.html > /dev/null
    echo "  subsequent requests (hit expected)"
    for i in {1..5}; do
        /usr/bin/time -f "  run $i elapsed: %e s" curl -s http://localhost:8080/index.html > /dev/null
    done
fi

echo "[test_load] done"
