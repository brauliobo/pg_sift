#!/usr/bin/env bash
# The rewrite worker of sift_col indexes by range (sift.col_auto_merge_partition_kb) and what bounds it: the worker cuts a churned index into partitions, then a
# window of churn in the newest rows is answered by the rewrite of the partition it reached and no other, in steps that let go of the maintenance lock between
# them; sift.col_auto_merge_max_workers keeps two workers of two indexes from rewriting at once; sift.maintenance_wal_rate makes the rewrite wait for the WAL
# it writes, so that the time of the rewrite has a lower bound set by the rate. Every answer is the heap's and the indexes verify.
. "$(dirname "$0")/lib.bash"

plan 12
node_init pw 26 "autovacuum = off
sift.log_min_duration = 0
sift.maintenance_naptime = 200"
node_start pw || bail "start failed"
D=$(dir pw)
sql pw "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
for t in fa fb; do
	sql pw "CREATE TABLE $t (id int, key int, kind text, amount numeric(10,2), pad text, tag text, tag2 text) WITH (autovacuum_enabled = false)" \
		"INSERT INTO $t SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 1000), md5(g::text), md5(g::text || 'b') FROM generate_series(1, 40000) g" \
		"CREATE INDEX ${t}_idx ON $t USING sift_col (id, key, kind, amount, tag, tag2) WITH (dead_rewrite_pct = 100)" "VACUUM $t" >/dev/null || bail "setup failed"
done
sql pw "ALTER DATABASE postgres SET sift.col_auto_merge = on" "ALTER DATABASE postgres SET sift.col_auto_merge_partition_kb = 128" \
	"ALTER DATABASE postgres SET sift.vacuum_rewrite_limit = 0" >/dev/null

# quiet: no maintenance worker runs. The snapshot of a worker that started before the DELETEs of a churn keeps VACUUM from removing the dead rows, so the index
# never learns of them and nothing is due (the same for the UPDATE of another table in the database); and the log a step measures from must hold all that
# the workers of the steps before it write
quiet() { for _ in $(seq 600); do [[ $(sql pw "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'") == 0 ]] && return 0; sleep 0.1; done; return 1; }
# churn_rows TABLE FROM M: of the rows from that id on, one of M updated and one of M deleted (other rows each time), 200 inserted; churn: and then VACUUM (the worker is asked for at its end)
n=0
churn_rows() { quiet; n=$((n + 1)); local r=$((n % $3)); sql pw "UPDATE $1 SET amount = amount + 1 WHERE id >= $2 AND id % $3 = $r" "DELETE FROM $1 WHERE id >= $2 AND id % $3 = ($r + $3 / 2) % $3" \
	"INSERT INTO $1 SELECT $n * 100000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 1000), md5(($n * 100000 + g)::text), md5(($n * 100000 + g)::text || 'b') FROM generate_series(1, 200) g" >/dev/null; }
churn() { churn_rows "$@"; sql pw "VACUUM (INDEX_CLEANUP ON) $1" >/dev/null; }
# state INDEX: partitions (immutable segments of at least half of sift.col_auto_merge_partition_kb, as the worker counts them: the overlays of a churn are smaller,
# and the rows of a partition depend on what the rows hold), segments and rows that are dirty, rows in the write buffer
state() { sql pw "SELECT count(*) FILTER (WHERE kind = 'immutable' AND bytes >= 128 * 512) || ' partitions, ' || count(*) FILTER (WHERE dirty) || ' dirty, ' || coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0) || ' buffered' FROM sift.col_segments('$1')"; }
settled() { [[ $(state "$1") =~ ^([0-9]+)\ partitions,\ 0\ dirty,\ 0\ buffered$ ]] && (( BASH_REMATCH[1] >= 6 )); }
await() { for _ in $(seq 600); do settled "$1" && return 0; sleep 0.1; done; return 1; }
gens() { sql pw "SELECT gen FROM sift.col_segments('$1') WHERE kind = 'immutable' AND bytes >= 128 * 512 ORDER BY gen"; }
groups() { sql pw "SET sift.enable_agg = $2" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM $1 GROUP BY kind) x" | tail -1; }
merges() { grep -c "sift: merged .* of index \"$1\"" "$D/postgres.log"; }
# since LINE: the log after that many lines; await_log PATTERN LINE: until PATTERN is logged after them
since() { tail -n +$(($1 + 1)) "$D/postgres.log"; }
await_log() { for _ in $(seq 600); do since "$2" | grep -q "$1" && return 0; sleep 0.1; done; return 1; }
verified() { test -z "$(sql pw "SELECT sift.col_verify('$1', true)" 2>&1)"; }

# 1. the churn of the whole table dirties every unit: the worker cuts the one segment into partitions
churn fa 0 100
check "the worker cuts the index into partitions, none dirty, nothing buffered" await fa_idx
check "the index verifies and the answers are the heap's" test "$(verified fa_idx && echo ok)|$(groups fa on)" == "ok|$(groups fa off)"

# 2. the churn of the newest 5% of the rows: only the partition it reached is rewritten
BEFORE=$(gens fa_idx)
M0=$(merges fa_idx)
churn fa 38000 4
for _ in $(seq 600); do [[ $(merges fa_idx) -gt $M0 ]] && break; sleep 0.1; done
await fa_idx
AFTER=$(gens fa_idx)
# the generations are numbers in numeric order, which comm (it wants the lexicographic order) misreads once they cross a power of ten
KEPT=$(grep -Fxc -f <(echo "$AFTER") <<<"$BEFORE")
diag "partitions $(wc -l <<<"$BEFORE") before, $KEPT kept, $(( $(merges fa_idx) - M0 )) merge steps"
check "the window of churn is rewritten: no dirty segment, nothing buffered" settled fa_idx
check "and the partitions it did not reach are the segments they were" test "$KEPT" -ge $(( $(wc -l <<<"$BEFORE") - 3 ))
check "the index verifies and the answers are the heap's" test "$(verified fa_idx && echo ok)|$(groups fa on)" == "ok|$(groups fa off)"

# 3. the rewrite of many partitions goes in steps (the lock is let go between them), none of them most of the rows: the documents each step rewrites are
# counted, not its time, which a busy host stretches. The units listed twice would take the index in one step (an overlay over every partition), so the rule
# for them is off here
sql pw "ALTER DATABASE postgres SET sift.col_auto_merge_fragmentation = 1000" >/dev/null
quiet
L0=$(wc -l <"$D/postgres.log")
churn fa 0 20
await_log "column rewrite worker finished index \"fa_idx\"" $L0
DOCS=$(since $L0 | grep "sift: merged .* of index \"fa_idx\"" | grep -o '[0-9]* documents' | grep -o '[0-9]*')
STEPS=$(wc -w <<<"$DOCS")
TOTAL=$(awk '{ s += $1 } END { print s + 0 }' <<<"$DOCS")
LONGEST=$(sort -n <<<"$DOCS" | tail -1)
diag "$STEPS steps, the largest $LONGEST documents of $TOTAL"
check "the partitions are rewritten in several steps, none of them most of the rows" test "$STEPS" -ge 5 -a "$(( ${LONGEST:-0} * 3 ))" -lt "$TOTAL"
await fa_idx
sql pw "ALTER DATABASE postgres RESET sift.col_auto_merge_fragmentation" >/dev/null

# 4. sift.col_auto_merge_max_workers: two workers for two indexes, one slot (an advisory lock of the key space 0x51F7, 0x53520000 + slot): never both at once
sql pw "ALTER DATABASE postgres SET sift.col_auto_merge_max_workers = 1" "ALTER DATABASE postgres SET sift.maintenance_cost_delay = 40" >/dev/null
slots() { sql pw "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND classid = 20983 AND objid >= 1397882880 AND granted"; }
# the rows of both tables change before either VACUUM: a snapshot of the database taken before a DELETE (the UPDATE of the other table, the worker the first VACUUM
# asked for) keeps the second VACUUM from removing the dead rows, and the index would have nothing to rewrite
churn_rows fa 0 20
churn_rows fb 0 20
sql pw "VACUUM (INDEX_CLEANUP ON) fa" >/dev/null &
cp=$!
sql pw "VACUUM (INDEX_CLEANUP ON) fb" >/dev/null
wait "$cp"
PEAK=0
for _ in $(seq 1200); do
	s=$(slots); (( s > PEAK )) && PEAK=$s
	settled fa_idx && settled fb_idx && break
	sleep 0.05
done
diag "most slots held at once: $PEAK"
check "both indexes are rewritten with a limit of one worker" settled fb_idx
check "never more than one worker held a slot, and one did" test "$PEAK" == 1
sql pw "ALTER DATABASE postgres RESET sift.col_auto_merge_max_workers" "ALTER DATABASE postgres RESET sift.maintenance_cost_delay" >/dev/null

# 5. sift.maintenance_wal_rate: the rewrite writes about as much WAL as the segments it writes are large, at no more than the rate
RATE=1000
sql pw "ALTER DATABASE postgres SET sift.maintenance_wal_rate = $RATE" >/dev/null
quiet
L0=$(wc -l <"$D/postgres.log")
churn fa 0 20
await_log "column rewrite worker finished index \"fa_idx\"" $L0
await fa_idx
MS=$(since $L0 | grep "rewrote (.*) the segments of index \"fa_idx\"" | grep -o 'in [0-9]* ms' | grep -o '[0-9]*' | awk '{ s += $1 } END { print s + 0 }')
KB=$(since $L0 | grep "sift: merged .* of index \"fa_idx\"" | grep -o '[0-9]* bytes' | awk '{ s += $1 } END { print int(s / 1024) }')
BOUND=$(( KB * 1000 / RATE * 6 / 10 ))
diag "the rewrite took ${MS:-?} ms for ${KB:-?} kB of segments at $RATE kB/s (at least $BOUND ms)"
check "the rewrite waited for the WAL rate: its time is over the bound of the bytes it wrote" test "${MS:-0}" -ge "$BOUND" -a "$BOUND" -ge 500
await fa_idx
sql pw "ALTER DATABASE postgres RESET sift.maintenance_wal_rate" >/dev/null
check "the index verifies and the answers are the heap's" test "$(verified fa_idx && echo ok)|$(groups fa on)" == "ok|$(groups fa off)"

# 6. without the settings the rewrite of one partition and of all are as before: partitions off, the worker rewrites the index into one segment
sql pw "ALTER DATABASE postgres RESET sift.col_auto_merge_partition_kb" >/dev/null
churn fa 0 20
big() { sql pw "SELECT count(*) FROM sift.col_segments('fa_idx') WHERE kind = 'immutable' AND rows > 15000"; }
for _ in $(seq 600); do [[ "$(big)|$(state fa_idx)" == "1|"*" 0 dirty, 0 buffered" ]] && break; sleep 0.1; done
check "partitions off: the worker rewrites all into one segment as before" test "$(big)" == 1
check "the index verifies and the answers are the heap's" test "$(verified fa_idx && echo ok)|$(groups fa on)" == "ok|$(groups fa off)"
finish
