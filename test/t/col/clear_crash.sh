#!/usr/bin/env bash
# A crash in the middle of the pass of a VACUUM that clears the liveness bits of the segments of an index (core/store.h P8). Each unit of a segment is cleared by a record of its own, so a crash
# keeps a prefix of the pass; a scope segment holds copies of rows of the entity segments, so the prefix must not end a row and keep its copy: sift.col_verify would find the row of a scope
# segment that is not in the entity segments (col_scope_kill_writes found it at random). sift.test_crash_clear = n (the testing build) kills the process after the nth segment the pass has
# visited, so every prefix of the pass is crashed in turn, with no dependence on timing: after the restart the index verifies against the heap, the scope reads are the join, and the VACUUM run
# again leaves the same. The same pass clears a sift_col index without a scope and a BM25 index (sift_mops_bulkdelete), which verify after each of their prefixes.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
. "${SIFT_REPO:?}/test/t/lib/twohop.bash"

node_init cc 54 "$TWOHOP_CONF
sift.async_maintenance = off
max_parallel_maintenance_workers = 0
shared_buffers = 64MB"
node_start cc || bail "start failed"
D=$(dir cc)
SIFT_TMP=$D leak_expect 20 'terminated by signal 9'
twohop_setup cc || bail "setup failed"
sql cc "CREATE TABLE docs (id int, g int, n int, body text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 6000) i" \
	"CREATE INDEX docs_col ON docs USING sift_col (id, g, n)" "CREATE INDEX docs_bm ON docs USING sift (body)" \
	"INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 3 FROM generate_series(6001, 7000) i" \
	"SELECT sift.col_promote('docs_col')" "SELECT sift.promote('docs_bm')" >/dev/null || bail "docs failed"

segments() { sql cc "SELECT count(*) FROM sift.col_segments('$1') WHERE kind <> 'tombstones'"; }
await_exit() { for _ in $(seq 600); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done; return 1; }
healthy() { # the indexes verify against their heaps, the scope has no differing key and its reads are the join
	[[ -z $(sql cc "SELECT sift.col_verify('hop_col', true)" "SELECT sift.col_verify('docs_col', true)" "SELECT sift.verify('docs_bm', true)" 2>&1) &&
		$(sql cc "SELECT count(*) FROM sift.col_scope_verify('hop_col', 1000) WHERE problem <> 'unverified'") == 0 &&
		$(sql cc "SELECT bool_and(hop_same(k)) FROM generate_series(1, 17) k" 2>&1 | tail -n 1) == t && $(twohop_reads cc) == same ]]
}
# crash VACUUM of TABLE after each segment of the pass of the index IX in turn, after DELETE of a new slice of rows
crash_each() { # LABEL TABLE IX DELETE
	local k n pm
	k=$(segments "$3")
	for n in $(seq "$k"); do
		sql cc "$4 $n" >/dev/null
		pm=$(head -1 "$D/data/postmaster.pid")
		sql cc "SET sift.test_crash_clear = $n" "VACUUM $2" >/dev/null 2>&1
		await_exit "$pm" || { not_ok "$1, after segment $n of $k: the process was not killed"; return 1; }
		node_start cc || { not_ok "$1, after segment $n of $k: restart"; diag "$(tail -20 "$D/postgres.log")"; return 1; }
		check "$1, killed after segment $n of $k: the indexes verify, the scope reads are the join" healthy
		sql cc "VACUUM $2" >/dev/null
		check "$1, after segment $n of $k: the VACUUM run again leaves the same" healthy
	done
}
crash_each "scope index" hop_events hop_col "DELETE FROM hop_events WHERE id % 13 ="
crash_each "plain column index and BM25" docs docs_col "DELETE FROM docs WHERE id % 13 ="
check "the log has no failed assertion" test "$(grep -c 'TRAP' "$D/postgres.log")" == 0
plan "$NTEST"
finish
