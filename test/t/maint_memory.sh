#!/usr/bin/env bash
# Promoting a write buffer stays within maintenance_work_mem whatever the size of the segment: a segment of 46 MB (limit
# 1 GB) is promoted at a 4 MB budget in a process that takes under 130 MB of anonymous memory; the docs of the segment used to
# be copied to memory, about six times its size (265 MB here, 800 MB for a segment of 138 MB).
. "$(dirname "$0")/lib.bash"

plan 3
node_init mem 11 "autovacuum = off"
node_start mem || bail "start failed"
sql mem "CREATE EXTENSION pg_sift" "CREATE TABLE t (id serial, body text)" \
	"CREATE INDEX t_idx ON t USING sift (body) WITH (write_buffer_limit = 1048576)" >/dev/null || bail "setup failed"
sql mem "SET sift.async_maintenance = off" \
	"INSERT INTO t (body) SELECT string_agg('w' || floor(power(random(), 3) * 100000)::int, ' ') FROM generate_series(1, 100000) g, generate_series(1, 40) j GROUP BY g" \
	>/dev/null || bail "load failed"
check "one write buffer of over 30 MB" test "$(sql mem "SELECT bytes > 30e6 AND kind = 'buffer' FROM sift.segments('t_idx')")" == t

D=$(dir mem)
PM=$(head -1 "$D/data/postmaster.pid")
rm -f "$D/stop"
(
	best=0
	while [[ ! -e $D/stop ]]; do
		for p in $(pgrep -P "$PM"); do
			kb=$(awk '/^RssAnon/ {print $2}' "/proc/$p/status" 2>/dev/null)
			(( ${kb:-0} > best )) && best=$kb
		done
		sleep 0.02
	done
	echo $((best / 1024)) >"$D/peak"
) &
POLL=$!
sql mem "SET maintenance_work_mem = '4MB'" "SELECT sift.promote('t_idx')" >/dev/null || bail "promote failed"
touch "$D/stop"
wait "$POLL"
diag "promotion peak: $(<"$D/peak") MB of anonymous memory in one process"
if [[ -z ${SIFT_PRELOAD:-} ]]; then
	check "the promotion stays under 130 MB at a 4 MB budget" test "$(<"$D/peak")" -lt 130
else
	skip 1 "the sanitizer's shadow memory and quarantine inflate the process (SANITIZE=1)"
fi
check "intact, every row indexed" test "$(sql mem "SELECT sift.verify('t_idx', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
finish
