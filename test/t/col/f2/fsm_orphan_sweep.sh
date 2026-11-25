#!/usr/bin/env bash
# The extents that a crash, or a failure between the write of a part and the commit that names it, leaves allocated and named by nothing go back to the allocator at the first VACUUM after
# the server starts (sift_fsm_sweep_orphans, core/f2/fsm/sweep.c): used_blocks falls by what leaked and the file is as healthy as before. The sweep never frees a block that something names: it
# runs only with the index to itself (a session that holds the index, and so may be between an allocation and its commit, makes it give up and a later VACUUM frees), and on indexes
# that hold a scope, projections, a key journal, deletion vectors, micro-slabs, pending deletes and column delta chains it frees nothing until a block is leaked, and then exactly that block.
# A leak is an extent the test functions allocate and never name, which is what a crash between the allocation and the commit leaves.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

ROWS=$(knob SIFT_SWEEP_ROWS 120000 60000)
RICH=(zr_col tenant_scope_journal p_idx dim_col ev_col)
plan $((4 + 4 + 3 * ${#RICH[@]} + 3))

p=osw
F2="SET sift.test_create_format1 = off"
node_init "$p" 72 "autovacuum = off
sift.async_maintenance = off"
node_start "$p" || bail "start failed"
sql "$p" "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "$F2" \
	"CREATE TABLE h (id int PRIMARY KEY, a bigint NOT NULL, b int NOT NULL, pad text NOT NULL)" \
	"INSERT INTO h SELECT g, g, g % 100, repeat('x', 30) FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX h_col ON h USING sift_col (id, a, b) WITH (track_changes = 'ctid')" \
	"UPDATE h SET a = a + 1 WHERE id % 7 = 0" "DELETE FROM h WHERE id % 11 = 0" >/dev/null || bail "setup failed"

used() { sql "$p" "SELECT used_blocks FROM sift.space('h_col')"; }
problems() { sql "$p" "SELECT problem FROM sift.check('$1', true)" 2>&1; }
leak() { sql "$p" "SELECT sift._test_f2_alloc('h_col', 5)" "SELECT count(*) FROM sift._test_f2_alloc_stream('h_col', 9)" "SELECT sift._test_f2_meta_alloc('h_col')" >/dev/null; }
LEAKED=14
agree() { # the plans give the same rows
	local a b
	a=$(sql "$p" "SET sift.enable_agg = on" "SELECT count(*), sum(a), sum(b) FROM h")
	b=$(sql "$p" "SET sift.enable_agg = off" "SELECT count(*), sum(a), sum(b) FROM h")
	out="$a | $b"
	[[ $a == "$b" ]]
}

# 1. a session that holds the index may be between an allocation and its commit: the sweep gives up, and frees when the session is gone
base=$(used)
coproc HOLD { psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock "$p")" -p "$(port "$p")" -U postgres -d "$(node_db "$p")"; }
printf '%s\n' "BEGIN;" "SELECT sift._test_f2_alloc('h_col', 5);" "SELECT count(*) FROM sift._test_f2_alloc_stream('h_col', 9);" "SELECT sift._test_f2_meta_alloc('h_col');" "\\echo READY" >&"${HOLD[1]}"
read -r -u "${HOLD[0]}" ready
read -r -u "${HOLD[0]}" ready
read -r -u "${HOLD[0]}" ready
read -r -u "${HOLD[0]}" ready
held=$(used)
check "the extents allocated by a session that has not committed are in use ($base -> $held blocks)" test "$held" -ge $((base + LEAKED))
sql "$p" "VACUUM h" >/dev/null
# the compaction of the VACUUM puts the blocks of the parts it replaced in the quarantine until the open transaction ends: they are no longer in use, and the leaked ones are
held_total=$(sql "$p" "SELECT used_blocks + pending_blocks FROM sift.space('h_col')")
check "a VACUUM while another session holds the index frees nothing ($held blocks in use, $held_total with the quarantine)" test "$held_total" -ge "$held"
printf '%s\n' "COMMIT;" '\q' >&"${HOLD[1]}"
wait "$HOLD_PID"
sql "$p" "VACUUM h" >/dev/null
check "the VACUUM that finds the index alone frees the leaked blocks ($held -> $(used) blocks in use)" test "$(used)" -le $((held - LEAKED))
out=$(problems h_col)
check "sift.check is clean after the sweep" test -z "$out"

# 2. a crash after an allocation: the first VACUUM after the restart frees it
sql "$p" "CHECKPOINT" >/dev/null
before=$(used)
leak
sql "$p" "CHECKPOINT" >/dev/null
leaked=$(used)
check "the blocks of an allocation that nothing names are in use ($before -> $leaked)" test "$leaked" -ge $((before + LEAKED))
crash "$p"
node_start "$p" || bail "the restart failed"
sql "$p" "VACUUM h" >/dev/null
after=$(used)
check "the first VACUUM after the restart frees them ($leaked -> $after blocks in use)" test "$after" -le $((leaked - LEAKED))
out=$(problems h_col)
check "sift.check is clean after the restart and the sweep" test -z "$out"
check "every plan gives the rows of the table" agree

# 3. the relations of every feature: a sweep frees nothing, then exactly what is leaked
sql "$p" "$F2" \
	"CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m % 25 <> 0" \
	"CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE zr (id int, grp int, dim_a_id int, v int) WITH (fillfactor = 10, autovacuum_enabled = false)" \
	"INSERT INTO zr SELECT i, i % 10, 1 + (i * 7919) % 300, i % 13 FROM generate_series(1, 40000) i" \
	"VACUUM (FREEZE, ANALYZE) zr, tenant_scope" \
	"CREATE INDEX zr_col ON zr USING sift_col (id, grp, dim_a_id, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) max_multiplicity(8)')" \
	"UPDATE zr SET v = v + 1 WHERE id % 5 = 0" "DELETE FROM zr WHERE id % 17 = 0" \
	"INSERT INTO zr SELECT 100000 + i, i % 10, 1 + (i * 31) % 300, i % 13 FROM generate_series(1, 300) i" \
	"CREATE TABLE p (id bigint, tenant int, status text, amount bigint)" \
	"CREATE INDEX p_idx ON p USING sift_col (id, tenant, status, amount) WITH (key_order = 'id')" \
	"INSERT INTO p SELECT i, i % 4, (ARRAY['new', 'old', 'mid'])[1 + i % 3], i % 10 FROM generate_series(1, 5966) i" "VACUUM p" \
	"SELECT sift.create_projection('p_idx', 'by_tenant', 'group by tenant, status measures count(*), sum(amount), min(amount), max(amount)')" \
	"SELECT sift.create_projection('p_idx', 'by_amount', 'order by amount, id include (tenant)')" \
	"INSERT INTO p SELECT 10000 + i, i % 4, 'new', i % 10 FROM generate_series(1, 500) i" \
	"UPDATE p SET amount = amount + 1 WHERE id % 9 = 0" "DELETE FROM p WHERE id % 13 = 0" \
	"CREATE TABLE dim (k int PRIMARY KEY, a int, b int, flag bool) WITH (autovacuum_enabled = false)" \
	"INSERT INTO dim SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 4999) i" \
	"CREATE INDEX dim_col ON dim USING sift_col (k, a, b, flag)" \
	"CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO ev SELECT i, CASE WHEN i % 211 = 0 THEN NULL ELSE i % 5100 END, i % 11 FROM generate_series(1, 40000) i" \
	"CREATE INDEX ev_col ON ev USING sift_col (id, k, v) WITH (connect = 'k -> dim(k)')" \
	"UPDATE dim SET a = a + 1 WHERE k % 50 = 0" >/dev/null || bail "the relations of every feature failed"
for i in "${RICH[@]}"; do
	check "$i: a sweep frees nothing that something names" test "$(sql "$p" "SELECT sift._test_f2_sweep('$i')")" == 0
	sql "$p" "SELECT sift._test_f2_alloc('$i', 3)" >/dev/null
	check "$i: a sweep after a leak of 3 blocks frees 3" test "$(sql "$p" "SELECT sift._test_f2_sweep('$i')")" == 3
	out=$(problems "$i")
	check "$i: sift.check is clean" test -z "$out"
done
sql "$p" "$F2" "CREATE TABLE ti (id int PRIMARY KEY, a int, b bigint, c int) USING sift_table" "CREATE TABLE tv (id int PRIMARY KEY, t text) USING sift_table" \
	"INSERT INTO ti SELECT g, g % 100, g, g % 7 FROM generate_series(1, 30000) g" "DELETE FROM ti WHERE id % 97 = 0" "UPDATE ti SET c = c + 1 WHERE id % 13 = 0" \
	"INSERT INTO tv SELECT g, repeat('w', g % 100) FROM generate_series(1, 1000) g" >/dev/null || bail "the tables failed"
check "a sift_table without a column of variable length is swept and nothing it names is freed" test "$(sql "$p" "SELECT sift._test_f2_sweep('ti')")" == 0
out=$(sql "$p" "SELECT problem FROM sift.check('ti', true)" 2>&1)
check "sift.check of the table is clean" test -z "$out"
check "a sift_table with a column of variable length (its wide values are named by a granule only) is not swept" test "$(sql "$p" "SELECT sift._test_f2_sweep('tv')")" == 0
finish
