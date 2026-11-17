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
ROLL+="@@SET sift.test_col_net = off@@SET sift.col_auto_merge = on@@SET sift.col_auto_merge_partition_kb = 512@@UPDATE docs SET g = g + 1 WHERE id % 7 = 0@@VACUUM docs@@SELECT sift.col_maintain('docs_ck')@@UPDATE docs SET g = g + 1 WHERE id % 5 = 0@@VACUUM docs"
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
	"col_roll~$ROLL~SET sift.test_col_net = off; SET sift.col_auto_merge = on; SET sift.col_auto_merge_partition_kb = 64; SELECT sift.col_maintain('docs_ck')"
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
fresh() { # SETUP
	sql cm "DROP TABLE IF EXISTS docs, tbl, tbl_h, gp CASCADE" "$BASE" >/dev/null || return 1
	[[ -z $1 ]] || { IFS=$'\x1f' read -ra stmts <<<"${1//@@/$'\x1f'}"; sql cm "${stmts[@]}" >/dev/null; } || return 1
	REF=$(answers PLAIN)
}
await_exit() { # PID
	for _ in $(seq 600); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done
	return 1
}

