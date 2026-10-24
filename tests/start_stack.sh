#!/bin/sh
# Starts the mock unit (:8765), the stand-in relay (:8790, auto-approve) and
# two unit relay sessions (A and B) for end-to-end tests.
cd "$(dirname "$0")"
for f in .pid_*; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null; rm -f "$f"; done
sleep 0.3
node mock.js > mock.log 2>&1 & echo $! > .pid_mock
(cd relay && AUTO=1 exec node relay.js > relay.log 2>&1) & echo $! > .pid_relay
sleep 1
cd relayhost && rm -f keyA.bin keyB.bin
./host_relay 8790 8765 ${SECS:-600} keyA.bin > unitA.log 2>&1 & echo $! > ../.pid_a
./host_relay 8790 8765 ${SECS:-600} keyB.bin > unitB.log 2>&1 & echo $! > ../.pid_b
