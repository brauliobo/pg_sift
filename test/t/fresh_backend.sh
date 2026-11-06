#!/usr/bin/env bash
# The fresh-backend mode of the regression tests (scripts/pgtest.sh fresh, scripts/fresh-psql.py, test/fresh/README.md): psql connects anew before every statement
# outside a transaction block, so a statement meets none of the caches, memory contexts and shared state an earlier statement of its backend left. The shim
# starts a connection for every statement, so it refuses to run inside itself or without the psql it stands for: a shim that found itself as that psql started
# itself without end (an out-of-memory host), and two fresh runs at once double the connections.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SHIM=$REPO/scripts/fresh-psql.py

plan 15
STUB=$(mktemp) && printf '#!/bin/sh\ncat\n' >"$STUB" && chmod +x "$STUB"
trap 'rm -f "$STUB"; cleanup' EXIT
# the script the shim hands to psql, for a script on stdin
fed() { FRESH_PSQL=$STUB "$SHIM" <<<"$1"; }
connects() { fed "$1" | grep -c '^\\connect$'; }

check "a statement outside a transaction block gets a connection of its own" test "$(connects $'SELECT 1;\nSELECT 2;')" == 2
check "the statements of a transaction block share one" test "$(connects $'BEGIN;\nSELECT 1;\nSELECT 2;\nCOMMIT;\nSELECT 3;')" == 2
check "from the first statement that binds the session (PREPARE) on there is no new connection" test "$(connects $'SELECT 1;\nPREPARE p AS SELECT 1;\nEXECUTE p;\nSELECT 2;')" == 2
check "the SET of the session is replayed after the connection" test "$(fed $'SET work_mem = 1000;\nSELECT 1;' | grep -c 'SET work_mem = 1000')" == 2
last_block() { fed "$1" | awk '/^\\connect$/ {buf = ""; next} {buf = buf $0 "\n"} END {printf "%s", buf}'; }
check "only the last SET of a setting is replayed (a SET ROLE of a role dropped since would fail)" test "$(last_block $'CREATE ROLE r;\nSET ROLE r;\nRESET ROLE;\nSET work_mem = 1000;\nSET work_mem = 2000;\nDROP ROLE r;\nSELECT 1;' | grep -cE 'ROLE|1000')|$(last_block $'SET work_mem = 1000;\nSET work_mem = 2000;\nSELECT 1;' | grep -c 'work_mem = 2000')" == "0|1"
check "the echo a new connection restores is the one the test set last" test "$(FRESH_PSQL=$STUB "$SHIM" -a <<<$'\\set ECHO none\nSELECT 1;' | grep -c 'ECHO all')" == 0
check "setseed binds the session: the random numbers after it come from its seed" test "$(connects $'SELECT setseed(0.5);\nSELECT random();\nSELECT random();')" == 1
check "FRESH_LOAD loads the library first in every new backend" test "$(FRESH_LOAD=pg_sift fed $'SELECT 1;\nSELECT 2;' | grep -c "LOAD ''pg_sift''")" == 2
FRESH_PSQL= "$SHIM" </dev/null >/dev/null 2>&1; r1=$?
FRESH_PSQL=$STUB FRESH_ACTIVE=1 "$SHIM" </dev/null >/dev/null 2>&1; r2=$?
FRESH_PSQL=$SHIM "$SHIM" </dev/null >/dev/null 2>&1; r3=$?
check "the shim refuses to run without the psql it stands for, inside itself, or as its own psql" test "$r1$r2$r3" == "111"
LOCKS=${XDG_RUNTIME_DIR:-/tmp}
check "no more fresh runs than SIFT_FRESH_RUNS" grep -q "at work" < <(exec 3>"$LOCKS/sift-fresh.0.lock" 4>"$LOCKS/sift-fresh.1.lock" 5>"$LOCKS/sift-fresh.2.lock" 6>"$LOCKS/sift-fresh.3.lock"; for fd in 3 4 5 6; do flock -n $fd || true; done; SIFT_FRESH_RUNS=4 "$REPO/scripts/pgtest.sh" fresh nothing 2>&1)
check "a fresh run does not start inside another" grep -q "do not nest" < <(FRESH_PSQL=$STUB "$REPO/scripts/pgtest.sh" fresh nothing 2>&1)

node_init fb 28 "autovacuum = off"
node_start fb || bail "start failed"
D=$(dir fb)
RP=$(command -v psql)
fresh() { FRESH_PSQL=$RP "$SHIM" -X -q -At -h "$D/sock" -p "$(port fb)" -U postgres -d postgres <<<"$1"; }
pids=$(fresh $'SELECT pg_backend_pid();\nSELECT pg_backend_pid();' | sort -u | wc -l)
check "two statements run in two backends" test "$pids" == 2
pids=$(fresh $'BEGIN;\nSELECT pg_backend_pid();\nSELECT pg_backend_pid();\nCOMMIT;' | grep -E '^[0-9]+$' | sort -u | wc -l)
check "the statements of a transaction block run in one" test "$pids" == 1
# the cold-backend class: the first statement of a backend loads the library and attaches what it shares; a backend that runs two statements has done that for the second
fresh $'CREATE EXTENSION pg_sift;\nCREATE TABLE ev (id int, k int);\nINSERT INTO ev SELECT g, g % 7 FROM generate_series(1, 5000) g;\nCREATE INDEX ev_i ON ev USING sift_col (id, k);\nVACUUM ev;' >/dev/null
warm=$(sql fb "SET sift.enable_agg = on" "SET sift.agg_force = on" "SELECT 'x', count(*) FROM ev GROUP BY k ORDER BY k" "SELECT 'scans', value FROM sift.stats() WHERE name = 'scans'" | tail -1)
cold=$(fresh $'SET sift.enable_agg = on;\nSET sift.agg_force = on;\nSELECT \'x\', count(*) FROM ev GROUP BY k ORDER BY k;\nSELECT \'scans\', value FROM sift.stats() WHERE name = \'scans\';' | tail -1)
check "a statement of a new backend sees none of the counters of the one before" test "$warm" != "$cold" -a "$cold" == "scans|0"
out=$(fresh $'SET work_mem = \'nope\';\nSELECT 1;\nSELECT 2;' 2>&1)
check "a SET the test saw fail does not fail again at every new connection" test "$(grep -c ERROR <<<"$out")" == 1
finish
