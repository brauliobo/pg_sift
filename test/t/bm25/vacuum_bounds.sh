#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# One VACUUM of a BM25 index stays within what maintenance_work_mem, autovacuum_work_mem and temp_file_limit allow, whatever the size of the index: a VACUUM someone runs merges the index to one segment
# (sift.vacuum_rewrite_limit holds an autovacuum pass back, not that VACUUM), writes temporary files within 2.5 times the limit (the segments it merges are streamed, not dumped to runs), holds memory that does not follow the
# rows (the same peak at 1x, 2x and 4x, slope at most 2 B/row of the counted scopes and 8 B/row of the resident peak, which counts the pages the rewrite touches), leaves the file within twice the live segments, copies at most
# the limit of the file in the space step, and a merge that fails, is cancelled or whose backend is terminated leaves no temporary file; an autovacuum worker takes its budget from autovacuum_work_mem.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 22
ROWS=$(knob SIFT_BMV_ROWS 50000 25000)
LIMIT=2
MB=1048576

node_init bb 58 "autovacuum = off
sift.async_maintenance = off
shared_buffers = 64MB
max_wal_size = 256MB
temp_file_limit = -1"
node_start bb || bail "start failed"
sql bb "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

# a table of N rows of 20 terms, vacuumed of its heap pages' bits; its BM25 index in SEGS segments is built by index_of
make_table() {
	local n=$1 name=$2
	sql bb "CREATE TABLE $name (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off)" \
		"INSERT INTO $name SELECT i, (SELECT string_agg('w' || ((i * 7 + j * 13) % 3000) || ' x' || ((i + j) % 500), ' ') FROM generate_series(1, 20) j) FROM generate_series(1, $n) i" \
		"VACUUM (INDEX_CLEANUP off, TRUNCATE off) $name" >/dev/null || bail "table $name failed"
}
index_of() {
	local name=$1 segs=${2:-16}
	sql bb "DROP INDEX IF EXISTS ${name}_i" "SET max_parallel_maintenance_workers = 0" \
		"CREATE INDEX ${name}_i ON $name USING sift (body) WITH (build_segments = $segs, vacuum_segments = 1)" >/dev/null || bail "index of $name failed"
}

declare -A M
# measure N: the VACUUM of the table of N rows in a backend of its own: ms, rss (kB), temp (bytes), exact (bytes of the memory scopes), rewritten, segs, live and file (bytes)
measure() {
	local n=$1 line
	make_table "$n" "t$n"
	index_of "t$n"
	sql bb "DELETE FROM t$n WHERE id % 3 = 0" >/dev/null || bail "delete failed"
	while IFS='|' read -r k v; do M[$n.$k]=$v; done < <(sql bb <<EOF
SET sift.async_maintenance = off; SET max_parallel_maintenance_workers = 0; SET maintenance_work_mem = '1MB'; SET sift.vacuum_rewrite_limit = 0;
CREATE TEMP TABLE before AS SELECT gen FROM sift.segments('t${n}_i');
SELECT pg_stat_force_next_flush();
SELECT temp_bytes AS t0 FROM pg_stat_database WHERE datname = current_database() \gset
SELECT sift._test_thp_disable();
SELECT count(*) FROM (SELECT sift._test_malloc_stats(true)) s;
SELECT sift._test_rss_peak_start() AS base \gset
SELECT clock_timestamp() AS ts0 \gset
VACUUM t$n;
SELECT 'ms', (extract(epoch FROM clock_timestamp() - :'ts0'::timestamptz) * 1000)::int;
SELECT 'rss', sift._test_rss_peak_stop() - :base;
SELECT pg_stat_force_next_flush();
SELECT 'temp', temp_bytes - :t0 FROM pg_stat_database WHERE datname = current_database();
SELECT 'exact', sum(peak) FROM sift._test_mem_highwater() WHERE parent IS NULL;
SELECT 'rewritten', coalesce(sum(bytes), 0) FROM sift.segments('t${n}_i') WHERE gen NOT IN (SELECT gen FROM before);
SELECT 'segs', count(*) FROM sift.segments('t${n}_i');
SELECT 'live', sum(bytes) FROM sift.segments('t${n}_i');
SELECT 'file', pg_relation_size('t${n}_i');
EOF
	)
	line="rows $n: ${M[$n.ms]} ms, rss ${M[$n.rss]} kB, exact ${M[$n.exact]} B, temp ${M[$n.temp]} B, rewrote ${M[$n.rewritten]} B, ${M[$n.segs]} segments ${M[$n.live]} B, file ${M[$n.file]} B"
	diag "$line"
}

R1=$ROWS R2=$((2 * ROWS)) R4=$((4 * ROWS))
for n in $R1 $R2 $R4; do measure "$n"; done
BOUND=$((LIMIT * MB))
out="rewritten $(for n in $R1 $R2 $R4; do printf '%s ' "${M[$n.rewritten]}"; done) bound $((2 * BOUND))"
check "one VACUUM merges the index to one segment at 1x, 2x and 4x (sift.vacuum_rewrite_limit 0: no limit)" \
	test "${M[$R1.segs]}" -eq 1 -a "${M[$R2.segs]}" -eq 1 -a "${M[$R4.segs]}" -eq 1
out="temp $(for n in $R1 $R2 $R4; do printf '%s ' "${M[$n.temp]}"; done) bound $((5 * BOUND / 2))"
check "its temporary files stay within 2.5 times the limit at 1x, 2x and 4x" \
	awk -v a="${M[$R1.temp]}" -v b="${M[$R2.temp]}" -v c="${M[$R4.temp]}" -v m=$((5 * BOUND / 2)) 'BEGIN { exit !(a <= m && b <= m && c <= m) }'
out="exact ${M[$R1.exact]} ${M[$R2.exact]} ${M[$R4.exact]}"
check "the memory it counts is the same at 4x as at 1x (at most 1.15 times)" \
	awk -v a="${M[$R1.exact]}" -v c="${M[$R4.exact]}" 'BEGIN { exit !(c <= 1.15 * a) }'
check "... and grows at most 2 B/row" \
	awk -v a="${M[$R1.exact]}" -v c="${M[$R4.exact]}" -v r=$((R4 - R1)) 'BEGIN { exit !((c - a) / r <= 2) }'
out="rss ${M[$R1.rss]} ${M[$R2.rss]} ${M[$R4.rss]} kB"
check "its resident peak grows at most 8 B/row" \
	awk -v a="${M[$R1.rss]}" -v c="${M[$R4.rss]}" -v r=$((R4 - R1)) 'BEGIN { exit !((c - a) * 1024 / r <= 8) }'
out="ms ${M[$R1.ms]} ${M[$R2.ms]} ${M[$R4.ms]}"
check "its time grows at most 5.5 times over 4x the rows" \
	awk -v a="${M[$R1.ms]}" -v c="${M[$R4.ms]}" 'BEGIN { exit !(c <= 5.5 * (a < 20 ? 20 : a)) }'
out="file ${M[$R1.file]} ${M[$R2.file]} ${M[$R4.file]} live ${M[$R1.live]} ${M[$R2.live]} ${M[$R4.live]}"
check "the file stays within twice the live segments at every size" \
	awk -v a="${M[$R1.file]}" -v b="${M[$R2.file]}" -v c="${M[$R4.file]}" -v x="${M[$R1.live]}" -v y="${M[$R2.live]}" -v z="${M[$R4.live]}" \
	'BEGIN { exit !(a <= 2 * x && b <= 2 * y && c <= 2 * z) }'

# the merge by several processes keeps to the budget: what its leader allocates (sift.test_build_peak) is within twice maintenance_work_mem at every size
declare -A Q
par_peak() {
	local n=$1
	make_table "$n" "q$n"
	index_of "q$n"
	Q[$n]=$(sql bb "SET sift.async_maintenance = off" "SET max_parallel_maintenance_workers = 2" "SET min_parallel_index_scan_size = 0" "SET sift.test_range_bytes = 50000" \
		"SET maintenance_work_mem = '1MB'" "SET sift.test_build_peak = 0" "SELECT sift.merge('q${n}_i', true)" "SELECT pg_size_bytes(current_setting('sift.test_build_peak'))" | tail -n 1)
}
for n in $R1 $R2 $R4; do par_peak "$n"; done
out="leader peak ${Q[$R1]} ${Q[$R2]} ${Q[$R4]} B"
check "the memory of a merge by several processes stays within twice maintenance_work_mem at 1x, 2x and 4x" \
	awk -v a="${Q[$R1]}" -v b="${Q[$R2]}" -v c="${Q[$R4]}" -v m=$((2 * MB)) 'BEGIN { exit !(a > 0 && a <= m && b <= m && c <= m) }'

# a term of every document is staged in files for what it needs: an index without positions whose terms occur once has no file for the checkpoints, the records or the frequencies of it
tmplog() { grep -c "temporary file:" "$(dir bb)/postgres.log"; }
emptylog() { grep -c "temporary file:.*size 0$" "$(dir bb)/postgres.log"; }
sql bb "SET max_parallel_maintenance_workers = 0" "CREATE TABLE p (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO p SELECT i, 'common u' || i || ' v' || (i * 7919 % 100003) FROM generate_series(1, 150000) i" \
	"CREATE INDEX p_i ON p USING sift (body) WITH (positions = false, build_segments = 4, vacuum_segments = 1)" >/dev/null || bail "table p failed"
files0=$(tmplog) empty0=$(emptylog)
sql bb "SET max_parallel_maintenance_workers = 0" "SET maintenance_work_mem = '1MB'" "SET log_temp_files = 0" "SELECT sift.merge('p_i', true)" >/dev/null || bail "merge of p failed"
out="temporary files of the merge: $(($(tmplog) - files0)), empty: $(($(emptylog) - empty0))"
check "a merge opens no temporary file that stays empty" test "$(($(emptylog) - empty0))" -eq 0
check "... and at most 5 (the live docs, the dictionary and its index, the blob of the term of every document)" test "$(($(tmplog) - files0))" -le 5

# the same with positions and frequencies above one, the merge of the staged blocks of positions: the index answers as before
sql bb "SET max_parallel_maintenance_workers = 0" "CREATE TABLE pp (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO pp SELECT i, 'common ' || CASE WHEN i % 3 = 0 THEN 'common ' ELSE '' END || 'u' || i || ' v' || (i * 7919 % 100003) FROM generate_series(1, 150000) i" \
	"CREATE INDEX pp_i ON pp USING sift (body) WITH (build_segments = 4, vacuum_segments = 1)" >/dev/null || bail "table pp failed"
ANS=$(sql bb "SELECT count(*) FROM pp WHERE body @@> 'common'")
sql bb "SET max_parallel_maintenance_workers = 0" "SET maintenance_work_mem = '1MB'" "SELECT sift.merge('pp_i', true)" >/dev/null || bail "merge of pp failed"
out="before $ANS after $(sql bb "SELECT count(*) FROM pp WHERE body @@> 'common'") verify $(sql bb "SELECT sift.verify('pp_i', true)")"
check "a merge with positions of a term of every document verifies and answers as before" \
	test "$(sql bb "SELECT count(*) FROM pp WHERE body @@> 'common'")" == "$ANS" -a -z "$(sql bb "SELECT sift.verify('pp_i', true)")"

# a merge that ends in an error, a cancel or a terminate leaves no temporary file or directory of files (the table of 8x the rows of 1x: merges that take a second), the merge by one process and by several
LEAK=$((8 * ROWS))
tmpfiles() { sql bb "SELECT count(*) FROM pg_ls_dir('base/pgsql_tmp', true, false)" | tail -n 1; }
rebuild() { index_of tl 48; }
merging() { [[ $(tmpfiles) -gt 0 ]]; }
backend() { sql bb "SELECT pid FROM pg_stat_activity WHERE datname = current_database() AND query LIKE '%sift.merge(%' AND pid <> pg_backend_pid()" | tail -n 1; }
settled() { [[ -z $(backend) ]]; }
bg_merge() { PGOPTIONS="-c maintenance_work_mem=1MB -c sift.async_maintenance=off $1" psql -X -q -h "$(node_sock bb)" -p "$(port bb)" -U postgres -d "$(node_db bb)" -c "SELECT sift.merge('tl_i', true) FROM generate_series(1, 6)" >/dev/null 2>&1 & }
make_table "$LEAK" tl
rebuild
out=$(sql bb "SET maintenance_work_mem = '1MB'" "SET temp_file_limit = '16kB'" "SELECT sift.merge('tl_i', true)" 2>&1)
check "a merge past temp_file_limit fails" grep -qE 'temporary file size exceeds "?temp_file_limit' <<<"$out"
check "... and leaves no temporary file" test "$(tmpfiles)" == 0

rebuild
bg_merge "-c max_parallel_maintenance_workers=0"
wait_for merging 120 || bail "the merge never wrote a temporary file"
sql bb "SELECT pg_cancel_backend($(backend))" >/dev/null
wait_for settled 120
check "a cancelled merge leaves no temporary file" test "$(tmpfiles)" == 0

rebuild
bg_merge "-c max_parallel_maintenance_workers=0"
wait_for merging 120 || bail "the merge never wrote a temporary file"
sql bb "SELECT pg_terminate_backend($(backend))" >/dev/null
wait_for settled 120
check "a terminated merge leaves no temporary file" test "$(tmpfiles)" == 0

rebuild
bg_merge "-c max_parallel_maintenance_workers=2 -c min_parallel_index_scan_size=0 -c sift.test_range_bytes=50000"
wait_for merging 120 || bail "the merge by workers never wrote a temporary file"
sql bb "SELECT pg_cancel_backend($(backend))" >/dev/null
wait_for settled 120
check "a cancelled merge by several processes leaves no temporary file" test "$(tmpfiles)" == 0

rebuild
bg_merge "-c max_parallel_maintenance_workers=2 -c min_parallel_index_scan_size=0 -c sift.test_range_bytes=50000"
wait_for merging 120 || bail "the merge by workers never wrote a temporary file"
sql bb "SELECT pg_terminate_backend($(backend))" >/dev/null
wait_for settled 120
check "a terminated merge by several processes leaves no temporary file" test "$(tmpfiles)" == 0
rebuild
sql bb "SET maintenance_work_mem = '1MB'" "SELECT sift.merge('tl_i', true)" >/dev/null
out=$(sql bb "SELECT sift.verify('tl_i', true)")
check "the next merge is sound" test -z "$out"

# the space step of a VACUUM copies at most the limit, a round of the lock at a time: the index file shrinks by what it copied, the WAL it writes is that copy
SLIMIT=1
space_scale() {
	local n=$1 name=s$1 before after wal
	make_table "$n" "$name"
	index_of "$name"
	sql bb "SELECT sift.merge('${name}_i', true)" >/dev/null || bail "merge failed"
	before=$(sql bb "SELECT pg_relation_size('${name}_i')")
	wal=$(sql bb <<EOF | tail -n 1
SET sift.async_maintenance = off; SET sift.vacuum_rewrite_limit = $SLIMIT; SET maintenance_work_mem = '4MB';
SELECT pg_current_wal_insert_lsn() AS l0 \gset
VACUUM $name;
SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'l0')::bigint;
EOF
	)
	after=$(sql bb "SELECT pg_relation_size('${name}_i')")
	M[$n.sbefore]=$before M[$n.safter]=$after M[$n.swal]=$wal
	diag "space rows $n: file $before -> $after B, WAL of the VACUUM $wal B"
}
S1=$((2 * ROWS)) S2=$((4 * ROWS)) S4=$((8 * ROWS))
for n in $S1 $S2 $S4; do space_scale "$n"; done
SBOUND=$((3 * SLIMIT * MB))
out="WAL ${M[$S1.swal]} ${M[$S2.swal]} ${M[$S4.swal]} bound $SBOUND"
check "the space step of one VACUUM copies no more than 3 times the limit at 1x, 2x and 4x (the WAL it writes)" \
	awk -v a="${M[$S1.swal]}" -v b="${M[$S2.swal]}" -v c="${M[$S4.swal]}" -v m=$SBOUND 'BEGIN { exit !(a <= m && b <= m && c <= m) }'
sql bb "SET sift.async_maintenance = off" "SET sift.vacuum_rewrite_limit = $SLIMIT" "VACUUM s$S4" >/dev/null
out="file ${M[$S4.safter]} then $(sql bb "SELECT pg_relation_size('s${S4}_i')")"
check "a second VACUUM goes on where the first stopped" test "$(sql bb "SELECT pg_relation_size('s${S4}_i')")" -le "${M[$S4.safter]}"
check "the index is intact after the rounds" test "$(sql bb "SELECT sift._test_check('s${S4}_i')")" == 0

# an autovacuum worker promotes the write buffer within autovacuum_work_mem, not within maintenance_work_mem
node_init av 59 "autovacuum = on
autovacuum_naptime = 1
autovacuum_vacuum_insert_threshold = 1000
autovacuum_work_mem = '1MB'
log_temp_files = 0
maintenance_work_mem = '1GB'
sift.async_maintenance = off
temp_file_limit = -1"
node_start av || bail "start of av failed"
sql av "CREATE EXTENSION pg_sift" "CREATE TABLE a (id int PRIMARY KEY, body text)" \
	"CREATE INDEX a_i ON a USING sift (body) WITH (write_buffer_limit = 262144)" >/dev/null || bail "setup of av failed"
t0=$(sql av "SELECT pg_stat_force_next_flush()" "SELECT temp_bytes FROM pg_stat_database WHERE datname = current_database()" | tail -n 1)
sql av "INSERT INTO a SELECT i, (SELECT string_agg('w' || ((i * 7 + j * 13) % 3000) || ' x' || ((i + j) % 500), ' ') FROM generate_series(1, 20) j) FROM generate_series(1, 40000) i" >/dev/null || bail "load failed"
vacuumed() { [[ -n $(sql av "SELECT last_autovacuum FROM pg_stat_user_tables WHERE relname = 'a'") ]]; }
wait_for vacuumed 120 || bail "autovacuum never vacuumed the table"
idle() { [[ $(sql av "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'autovacuum worker'") == 0 ]]; }
wait_for idle 120
t1=$(sql av "SELECT pg_stat_force_next_flush()" "SELECT temp_bytes FROM pg_stat_database WHERE datname = current_database()" | tail -n 1)
out="temp bytes before $t0 after $t1; segments $(sql av "SELECT count(*) FROM sift.segments('a_i')")"
check "the promotion of an autovacuum worker sorts within autovacuum_work_mem (its sort spills at 1 MB, not at 1 GB: over 1 MB of temporary files)" test "$((t1 - t0))" -ge "$MB"
finish
