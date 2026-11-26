#!/usr/bin/env bash
# A shortage of dynamic shared memory (no room in /dev/shm, no free segment) is never an error for what asks for a shared table where sift_dsm_attach cannot catch it in a subtransaction:
# a parallel operation (the directory and ring of the dictionaries), a statement that writes (the pool of the write buffers' references), a background worker (the
# queue of the rollup refresh). sift_dsm_try (col/dsmreg.c) catches the shortage where the segment is made, gives the attempt a resource owner of its own (no "dynamic shared memory
# leak" warning at the commit) and returns no table; each caller goes without it. sift.test_dsm_fail makes the segment of a table whose name has the given part fail with the error a full
# /dev/shm raises, so the tables are the ones of a server that never had them: a fresh cluster, the failing cases first. Format 1: the dictionary ring and the arena of the write buffers are
# Format 1's.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 15

node_init sh 53 "sift.test_create_format1 = on
autovacuum = off
sift.test_dsm_fail = 'bgworker:rollup'"
node_start sh || bail "start failed"
D=$(dir sh)
sql sh "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "LOAD 'pg_sift'" "SET sift.test_dsm_fail = 'sift_col_wbuf_arena'" "CREATE TABLE w (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX w_c ON w USING sift_col (id, g, v) WITH (track_changes = 'id')" "INSERT INTO w SELECT i, i % 7, i FROM generate_series(1, 20) i" >/dev/null || bail "setup failed"
[[ $(sql sh "SELECT count(*) FROM sift._test_dsm_entries() WHERE name LIKE 'sift_col_wbuf_arena%'") == 0 ]] || bail "the arena exists before the cases ask for it"

# fails PART CALLER PARALLEL: a new backend whose segment for PART fails, asks as CALLER does; its answer (t when it got the table), the lines it wrote on stderr in $D/err
fails() { PGOPTIONS="-c client_min_messages=warning" sql sh "LOAD 'pg_sift'" "SET sift.test_dsm_fail = '$1'" "SELECT sift._test_dsm_caller('$2', $3)" 2>"$D/err"; }
logged() { grep -c "$1" "$D/postgres.log" || true; }
entries() { sql sh "SELECT count(*) FROM sift._test_dsm_entries() WHERE name LIKE '%$1%'"; }

check "dictionary_worker: no table, no error" test "$(fails sift_col_dict_5 dictionary_worker true)|$(cat "$D/err")|$(entries sift_col_dict)" == "f||0"
check "a backend that went without a table makes the tables of other names (nothing is left held)" \
	test "$(sql sh "LOAD 'pg_sift'" "SET sift.test_dsm_fail = 'sift_col_dict_5'" "SELECT sift._test_dsm_caller('dictionary_worker', true)" "RESET sift.test_dsm_fail" "SELECT sift._test_dsm_attach('shortage_probe', 1024)" 2>&1 | paste -sd' ' -)" == "f t"

# the same asks with room: the tables are made
check "dictionary_worker with room: the table is made" test "$(fails none dictionary_worker true)|$(entries sift_col_dict)" == "t|1"

# a statement that writes: the pool of the references of the write buffers has no room; the rows carry their values
write_rows() { PGOPTIONS="-c client_min_messages=warning" sql sh "LOAD 'pg_sift'" "SET sift.test_dsm_fail = '$1'" "INSERT INTO w SELECT i, i % 7, i FROM generate_series($2, $3) i" 2>"$D/err"; }
answers_agree() {
	[[ $(sql sh "SET sift.enable_agg = on" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(g || ':' || n || ':' || s, ' ' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM w GROUP BY g) q" | tail -1) == \
		$(sql sh "SET sift.enable_agg = off" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(g || ':' || n || ':' || s, ' ' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM w GROUP BY g) q" | tail -1) ]]
}
L0=$(logged "references of the write buffers are not shared")
write_rows sift_col_wbuf_arena 21 3000
check "an INSERT of a backend whose first row finds no room for the arena: the statement ends, with no error" test "$(cat "$D/err")|$(sql sh "SELECT count(*) FROM w")" == "|3000"
check "... the log said it once for the postmaster (the first backend, at the setup), and no arena was made" test "$(($(logged "references of the write buffers are not shared") - L0))|$(entries sift_col_wbuf_arena)" == "0|0"
check "... sift.shared_memory() says why the arena is not shared" test "$(sql sh "SELECT shared || ' ' || (reason LIKE '%No space left%') FROM sift.shared_memory() WHERE name = 'sift_col_wbuf_arena'")" == "false true"
check "... the aggregate through the index is the heap's" answers_agree
# the arena is made, and no room for the first segment of its pool: the rows carry their values, and the backend asks again a second later, not at each row
L0=$(logged "pool of the write buffer references takes no more memory")
write_rows _seg 3001 6000
check "no room for a segment of the pool: the statement ends, with no error, one line in the log, the arena without a segment" \
	test "$(cat "$D/err")|$(sql sh "SELECT count(*) FROM w")|$(($(logged "pool of the write buffer references takes no more memory") - L0))|$(entries wbuf_arena_4)|$(entries wbuf_arena_4_seg)" == "|6000|1|1|0"
check "... the aggregate through the index is the heap's" answers_agree
write_rows none 6001 9000
check "a backend with room makes the segment of the pool and writes; the heap and the index agree" test "$(cat "$D/err")|$(sql sh "SELECT count(*) FROM w")|$(entries wbuf_arena_4_seg0)" == "|9000|1" && answers_agree

# the rollup refresh worker: no table is there for it (no request made one), and none can be made for it
L0=$(logged "the rollup refresh worker has no queue to serve")
check "the worker is started" test "$(sql sh "SELECT sift._test_col_rollup_worker()")" == t
wait_for '[[ $(logged "the rollup refresh worker has no queue to serve") -gt '"$L0"' ]]' 60
check "a rollup refresh worker that cannot make its table says so in the log and ends" test "$(($(logged "the rollup refresh worker has no queue to serve") - L0))" == 1
wait_for '[[ $(sql sh "SELECT count(*) FROM pg_stat_activity WHERE backend_type = '"'"'sift rollup refresh'"'"'") == 0 ]]' 30
check "... no worker is left, and none ended with an error" test "$(sql sh "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift rollup refresh'")|$(grep -c 'ERROR:.*rollup' "$D/postgres.log" || true)" == "0|0"
check "no shared table is left half made: the entries are the ones of the successful asks" \
	test "$(sql sh "SELECT string_agg(name, ' ' ORDER BY name) FROM sift._test_dsm_entries() WHERE name ~ '(arena|probe|dict_5)'")" == "shortage_probe sift_col_dict_5 sift_col_wbuf_arena_4 sift_col_wbuf_arena_4_seg0"
check "the server is up" test "$(sql sh "SELECT 1")" == 1
finish
