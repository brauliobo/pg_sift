# The scenarios of the crash and fault matrices (col_crash_matrix.sh, fault_matrix.sh) on the node "cm": the table docs and the operations that publish a directory of an index, the
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# verification of a healthy state (every sift index verifies against its heap and passes the store check, the answers through the indexes are the plain plan's).
PLAIN=("SET sift.enable_agg = off" "SET sift.enable_scan = off" "SET sift.enable_count = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off")
IDX=("SET sift.enable_agg = on" "SET seq_page_cost = 10000" "SET enable_seqscan = off")
QA="SELECT g, count(*), sum(n), min(id), max(id) FROM docs GROUP BY g ORDER BY g"
QB="SELECT count(*), sum(id) FROM docs WHERE body @@> 'alpha3 beta4'"
QT="SELECT count(*), sum(id), sum(g * 3 + n), count(DISTINCT id), count(DISTINCT n), sift.approx_count_distinct(id) FROM"
QO="SELECT string_agg(id || ':' || g || ':' || n, ',' ORDER BY g DESC, n, id) FROM (SELECT id, g, n FROM docs ORDER BY g DESC, n, id LIMIT 200) x"
BASE="CREATE TABLE docs (id int, g int, n int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 6000) i"
more() { echo "INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 3 || ' beta' || i % 7 FROM generate_series($1, $2) i"; }
BM25="CREATE INDEX docs_idx ON docs USING sift (body) WITH (write_buffer_limit = 4096)"
COL="CREATE INDEX docs_col ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, cube_minmax = true, key_columns = 'g, n', hll_columns = 'id')"
# an index with a scope: the member g of docs has one to three keys t in the relation gp (with its journal index; the refill is of entries that an edit of the pairs made stale); scope_missing() counts the rows of the entity segments (id <= $1: those not in a write buffer) that lack an entry
GP="CREATE TABLE gp (g int, t int, PRIMARY KEY (g, t)); INSERT INTO gp SELECT g, 1 + (g * 5 + j * 7) % 6 FROM generate_series(0, 8) g, generate_series(0, g % 3) j; CREATE INDEX gp_journal ON gp USING sift_col (g, t) WITH (track_changes = 'ctid')"
SCOPEIX="CREATE INDEX docs_sc ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, scope = 't: g -> gp(g, t)')"
SCOPE="$GP; $SCOPEIX"
scope_check() { # the highest id of a row that is in an entity segment
	echo "CREATE OR REPLACE FUNCTION scope_missing() RETURNS bigint LANGUAGE plpgsql AS \$f\$ BEGIN RETURN (SELECT count(*) FROM (SELECT d.ctid, p.t FROM docs d JOIN gp p USING (g) WHERE d.id <= $1 EXCEPT SELECT tid, key::int FROM sift._test_col_scope_entries('docs_sc')) m); END \$f\$"
}
# a key_cut index in partitions of 512 kB whose rolling rewrite then runs at 64 kB: each partition is a step that writes its window as several pieces in one switch of the directory (statements of a setup split at @@: a VACUUM
# cannot be in a transaction)
ROLL="INSERT INTO docs SELECT i, i % 9, i % 100, md5(i::text) FROM generate_series(6001, 100000) i@@CREATE INDEX docs_ck ON docs USING sift_col (id, g, n, body) WITH (key_cut = 'n', dead_rewrite_pct = 100)"
ROLL+="@@SET sift.test_col_net = off@@SET sift.col_auto_merge = on@@SET sift.test_col_partition_kb = 512@@UPDATE docs SET g = g + 1 WHERE id % 7 = 0@@VACUUM docs@@SELECT sift.col_maintain('docs_ck')@@UPDATE docs SET g = g + 1 WHERE id % 5 = 0@@VACUUM docs"
TBL="CREATE TABLE tbl (id int, g int, n int) USING sift_table WITH (autovacuum_enabled = false); CREATE TABLE tbl_h (id int, g int, n int)"

# scenario ~ setup (after the base table and its rows) ~ the operation
SCENARIOS=(
	"bm25_build~~$BM25"
	"col_build~~$COL"
	"bm25_promote~$BM25; $(more 6001 8000)~SELECT sift.promote('docs_idx')"
	"col_promote~$COL; $(more 6001 8000)~SELECT sift.col_promote('docs_col')"
	"bm25_merge~$BM25; $(more 6001 7000); SELECT sift.promote('docs_idx'); $(more 7001 8000); SELECT sift.promote('docs_idx')~SELECT sift.merge('docs_idx', true)"
	"col_merge~$COL; $(more 6001 7000); SELECT sift.col_promote('docs_col'); $(more 7001 8000); SELECT sift.col_promote('docs_col')~SELECT sift.col_merge('docs_col', true)"
	"scope_build~$GP; $(scope_check 6000)~$SCOPEIX"
	"scope_promote~$SCOPE; $(scope_check 6000); $(more 6001 8000)~SELECT sift.col_promote('docs_sc')"
	"scope_merge~$SCOPE; $(scope_check 8000); $(more 6001 7000); SELECT sift.col_promote('docs_sc'); $(more 7001 8000); SELECT sift.col_promote('docs_sc')~SELECT sift.col_merge('docs_sc', true)"
	"scope_refill~$SCOPE; $(scope_check 8000); $(more 6001 8000); SELECT sift.col_promote('docs_sc')~UPDATE gp SET t = t; SELECT sift.col_maintain('docs_sc')"
	"vacuum~$BM25; $COL; DELETE FROM docs WHERE id % 3 = 0; UPDATE docs SET n = n + 1 WHERE id % 7 = 0~VACUUM docs"
	"col_net~$COL; DELETE FROM docs WHERE id % 3 = 0; UPDATE docs SET n = n + 1 WHERE id % 7 = 0~VACUUM docs"
	"col_roll~$ROLL~SET sift.test_col_net = off; SET sift.col_auto_merge = on; SET sift.test_col_partition_kb = 64; SELECT sift.col_maintain('docs_ck')"
	"tbl_seal~$TBL; INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(1, 9000) i; INSERT INTO tbl_h SELECT i, i % 9, i % 100 FROM generate_series(1, 9000) i; DELETE FROM tbl WHERE id % 5 = 0; DELETE FROM tbl_h WHERE id % 5 = 0~VACUUM tbl"
)

# healthy: every sift index verifies against its heap and passes the store check
VERIFY="DO \$\$ DECLARE r record; BEGIN
	FOR r IN SELECT c.oid::regclass AS ix, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col') LOOP
		IF r.amname = 'sift' THEN PERFORM sift.verify(r.ix, true); ELSE PERFORM sift.col_verify(r.ix, true); END IF;
		PERFORM sift._test_check(r.ix);
	END LOOP;
	IF to_regclass('docs_sc') IS NOT NULL AND to_regprocedure('scope_missing()') IS NOT NULL THEN
		IF scope_missing() > 0 THEN RAISE EXCEPTION 'a row of an entity segment has no scope entry'; END IF;
	END IF;
END \$\$"
# the answers of everything the scenario created, in a mode
answers() {
	local -n mode=$1
	local has qs=("$QA" "$QB")
	has=$(sql cm "SELECT string_agg(relname, ',') FROM pg_class WHERE relname IN ('docs', 'tbl')")
	[[ $has != *docs* ]] || qs+=("$QT docs" "$QO")
	[[ $has != *tbl* ]] || qs+=("$QT tbl" "$QT tbl_h")
	sql cm "${mode[@]}" "${qs[@]}" 2>&1
}
healthy() { # the indexes verify and the answers through them are the plain plan's and REF
	local plain
	sql cm "$VERIFY" >&2 || return 1
	plain=$(answers PLAIN)
	[[ $plain == "$REF" ]] || { diag "the plain answers changed: $plain"; return 1; }
	[[ $(answers IDX) == "$plain" ]] || { diag "the answers through the indexes differ from the plain plan"; return 1; }
	twins=$(sql cm "SELECT count(*) FROM pg_class WHERE relname = 'tbl'")
	[[ $twins == 0 ]] || [[ $(sql cm "$QT tbl") == "$(sql cm "$QT tbl_h")" ]] || { diag "the sift_table differs from its heap twin"; return 1; }
}
fresh() { # SETUP: statements split at @@ run in one session; a last one !crash stops the server immediately and starts it again
	local setup=$1 crash=
	[[ $setup != *'@@!crash' ]] || { setup=${setup%@@!crash}; crash=1; }
	sql cm "DROP TABLE IF EXISTS docs, tbl, tbl_h, gp CASCADE" "$BASE" >/dev/null || return 1
	[[ -z $setup ]] || { IFS=$'\x1f' read -ra stmts <<<"${setup//@@/$'\x1f'}"; sql cm "${stmts[@]}" >/dev/null; } || return 1
	[[ -z $crash ]] || { pg_ctl -D "$D/data" -m immediate -w stop >/dev/null 2>&1; node_start cm || return 1; }
	REF=$(answers PLAIN)
}
await_exit() { # PID
	for _ in $(seq 600); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done
	return 1
}


# the gates: GATE_SET (statements of the session that runs the operation), GATE_LOCK (what the holder session takes) and GATE_COND (the row of pg_locks of that advisory lock)
adv2() { GATE_LOCK="pg_advisory_lock(20983, $1)"; GATE_COND="classid = 20983 AND objid = $1 AND objsubid = 2"; }
adv1() { GATE_LOCK="pg_advisory_lock($1)"; GATE_COND="classid = 0 AND objid = $1 AND objsubid = 1"; }
gate_spec() { # GATE: the gate of the phase "gate:GATE"
	GATE_SET=()
	case $1 in
		dir) adv2 13 ;;
		build) adv2 8; GATE_SET=("SET sift.test_build_gate = on") ;;
		drop) adv2 9; GATE_SET=("SET sift.test_drop_gate = on") ;;
		cleanup) adv2 2; GATE_SET=("SET sift.test_cleanup_gate = on") ;;
		rebuild) adv2 11; GATE_SET=("SET sift.test_rebuild_gate = on" "SET sift.agg_force = on" "SET sift.col_wbuf_ref = on" "SET max_parallel_workers_per_gather = 0") ;;
		intern) adv2 12; GATE_SET=("SET sift.test_intern_gate = on") ;;
		bulkdelete) adv1 100; GATE_SET=("SET sift.test_bulkdelete_gate = 100") ;;
		tail) adv1 7004; GATE_SET=("SET sift.test_phase_gate = 7000") ;;
		fill) adv1 7005; GATE_SET=("SET sift.test_phase_gate = 7000") ;;
		journal) adv1 7006; GATE_SET=("SET sift.test_phase_gate = 7000") ;;
		load) adv1 7004; GATE_SET=("SET sift.test_phase_gate = 7000" "SET max_parallel_maintenance_workers = 2" "SET maintenance_work_mem = '256MB'" "SET sift.test_load_unit_bytes = 128") ;;
		tombrewrite) adv1 7007; GATE_SET=("SET sift.test_phase_gate = 7000") ;;
		tombwrite) adv1 7008; GATE_SET=("SET sift.test_phase_gate = 7000" "SET sift.backend_cache_kb = 16") ;;
		tombmerge) adv1 7009; GATE_SET=("SET sift.test_phase_gate = 7000" "SET sift.backend_cache_kb = 16") ;;
		alter) adv2 14; GATE_SET=("SET sift.test_alter_gate = on") ;;
		repair_before) adv2 15; GATE_SET=("SET sift.test_repair_gate = on") ;;
		repair_after) adv2 16; GATE_SET=("SET sift.test_repair_gate = on") ;;
	esac
}

# the statements of an operation (split at @@) in OPS: statements that cannot share a transaction block (a VACUUM after a SET) are separate commands of one session
op_stmts() { IFS=$'\x1f' read -ra OPS <<<"${OP[$1]//@@/$'\x1f'}"; }

# the scenarios of the operations of the later rounds (they name "$D", the directory of the node)
CIC_OPTS="WITH (write_buffer_limit = 4096)"
SCENARIOS+=(
	"col_cic~~CREATE INDEX CONCURRENTLY docs_col ON docs USING sift_col (id, g, n) $CIC_OPTS"
	"track_cic~~CREATE INDEX CONCURRENTLY docs_tr ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, track_changes = 'ctid')"
	"scope_cic~$GP~CREATE INDEX CONCURRENTLY docs_sc ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, scope = 't: g -> gp(g, t)')"
	"scope_cic_nj~${GP%%; CREATE INDEX gp_journal*}~CREATE INDEX CONCURRENTLY docs_sc ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, scope = 't: g -> gp(g, t)')"
	"reindex_col~$COL~REINDEX INDEX docs_col"
	"reindex_col_c~$COL~REINDEX INDEX CONCURRENTLY docs_col"
	"drop_track~CREATE INDEX docs_tr ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, track_changes = 'ctid')~DROP INDEX IF EXISTS docs_tr"
	"tbl_tomb~$TBL@@INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(1, 9000) i@@INSERT INTO tbl_h SELECT i, i % 9, i % 100 FROM generate_series(1, 9000) i~UPDATE tbl SET n = n + 1; UPDATE tbl_h SET n = n + 1"
	"reindex_scope_c~$SCOPE~REINDEX INDEX CONCURRENTLY docs_sc"
	"alter_refresh~$COL@@ALTER INDEX docs_col SET (cube_minmax = false)~SELECT * FROM sift.col_refresh('docs_col')"
	"alter_reloption~$COL~ALTER INDEX docs_col SET (write_buffer_limit = 8192)"
	"col_repair~CREATE INDEX docs_col ON docs USING sift_col (id, g, n)@@DELETE FROM docs WHERE id % 7 = 0@@VACUUM (INDEX_CLEANUP ON) docs@@SELECT sift._test_poke('docs_col', first_block, 32, 8, 1) FROM sift.col_extents('docs_col') WHERE what = 'liveness run'@@SELECT sift._test_poke('docs_col', first_block, 8178, 4, 2) FROM sift.col_extents('docs_col') WHERE what = 'liveness run'@@CHECKPOINT~SELECT count(*) FROM sift.col_repair('docs_col')"
	"tbl_copy~$TBL@@COPY (SELECT i, i % 9, i % 100 FROM generate_series(1, 9000) i) TO '$D/load.csv' CSV~COPY tbl FROM '$D/load.csv' CSV; COPY tbl_h FROM '$D/load.csv' CSV"
)
# deletion records (core/v1_col/tomb_dv.h): a committed statement deletes a fifth of the rows of an index that tracks its changes, so that the log holds records; the VACUUM and the fold clear their rows and drop them, the merge carries them to marks
DVSET="ALTER TABLE docs ADD PRIMARY KEY (id); CREATE INDEX docs_col ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, track_changes = 'id')"
SCENARIOS+=(
	"dv_vacuum~$DVSET; DELETE FROM docs WHERE id % 5 = 0~VACUUM docs"
	"dv_fold~$DVSET; DELETE FROM docs WHERE id % 5 = 0~SELECT sift._test_col_fold_marks('docs_col')"
	"dv_merge~$DVSET; $(more 6001 7000); SELECT sift.col_promote('docs_col'); $(more 7001 8000); SELECT sift.col_promote('docs_col'); DELETE FROM docs WHERE id % 5 = 0~SELECT sift.col_merge('docs_col', true)"
)
DATA_OPS=" tbl_tomb tbl_copy "
DAMAGED=" col_repair "
