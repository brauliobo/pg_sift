#!/usr/bin/env bash
# Real released libraries write the input, including unpromoted records. No fixture produced by the new writer can prove this upgrade.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots; none supports 19 yet)'; exit 0; fi
for ref in v3.0.1 v3.1.0; do
	if ! git -C "$REPO" rev-parse --verify -q "$ref^{commit}" >/dev/null; then
		echo "1..0 # SKIP released library source $ref is unavailable"
		exit 0
	fi
done
newroot=$ROOT
use_root() {
	ROOT=$1
	node_conf legacy "extension_control_path = '$ROOT$(pg_config --sharedir):\$system'
dynamic_library_path = '$ROOT$(pg_config --pkglibdir):\$libdir'"
}
QUERY="SELECT count(*) FILTER (WHERE jsonb_path_exists(items, '\$[*] ? (@.s != \"gift\")')) || ':' ||
 count(*) FILTER (WHERE jsonb_path_exists(items, '\$[*] ? (@.s == null)')) || ':' ||
 count(*) FILTER (WHERE items @> '[{\"s\":null}]') FROM oldcol"
TYPES="SELECT string_agg(coalesce(t, 'missing') || ':' || n, ',' ORDER BY t NULLS FIRST)
 FROM (SELECT jsonb_typeof(attr->'a') t, count(*) n FROM oldcol GROUP BY 1) s"
for ref in v3.0.1 v3.1.0; do
	oldroot=$(SIFT_OLD_REF=$ref "$REPO/scripts/old-lib-check.sh" build) || bail "cannot build $ref"
	node_init legacy 2 'autovacuum = off
sift.async_maintenance = off'
	use_root "$oldroot"
	textkind=astext
	[[ $ref != v3.0.1 ]] || textkind=text
	node_start legacy || bail "cannot start $ref"
	sql legacy "CREATE EXTENSION pg_sift" "CREATE TABLE oldcol (id int, items jsonb, attr jsonb)" \
		"CREATE INDEX oldcol_idx ON oldcol USING sift_col (id,items,attr) WITH (paths = 'items[]{s text}, attr->a $textkind')" \
		"INSERT INTO oldcol SELECT i, jsonb_build_array(CASE i % 3 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{\"s\":null}'::jsonb ELSE '{\"s\":\"gift\"}'::jsonb END),
		 CASE i % 5 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{\"a\":null}'::jsonb WHEN 2 THEN '{\"a\":true}'::jsonb WHEN 3 THEN '{\"a\":1}'::jsonb ELSE '{\"a\":\"1\"}'::jsonb END
		 FROM generate_series(1,90) i" "VACUUM ANALYZE oldcol" >/dev/null || bail "$ref setup"
	if [[ $ref == v3.1.0 ]]; then
		sql legacy "CREATE TABLE oldseg AS TABLE oldcol" "CREATE INDEX oldseg_idx ON oldseg USING sift_col (id,items,attr) WITH (paths = 'items[]{s text}, attr->a astext')" \
			"VACUUM ANALYZE oldseg" "CREATE TABLE oldtbl (LIKE oldcol) USING sift_table" \
			"INSERT INTO oldtbl SELECT * FROM oldcol" "VACUUM oldtbl" >/dev/null || bail '3.1 immutable setup'
	fi
	want=$(sql legacy "SET sift.enable_agg = off" "$QUERY")
	types=$(sql legacy "SET sift.enable_agg = off" "$TYPES")
	# The small INSERT stays in the old library's write buffer, never an immutable segment.
	check "$ref left unpromoted records" test "$(sql legacy "SELECT count(*) FROM sift.col_segments('oldcol_idx') WHERE kind = 'immutable'")" == 0
	node_stop legacy
	use_root "$newroot"
	node_start legacy || bail 'cannot start new library'
	sql legacy "ALTER EXTENSION pg_sift UPDATE" >/dev/null || bail 'SQL update'
	if [[ $ref == v3.1.0 ]]; then
		sql legacy "SELECT sift.table_set_paths('oldtbl','items','items[]{s text}')" \
			"SELECT sift.table_set_paths('oldtbl','attr','attr->a astext')" >/dev/null || bail 'declare upgraded paths'
		check 'real 3.1 minor 4 index null predicates equal the heap' test "$(sql legacy "SET seq_page_cost = 10000" "${QUERY//oldcol/oldseg}")" == "$want"
		check 'real 3.1 table groups preserve JSON null answers' test "$(sql legacy "SET seq_page_cost = 10000" "${QUERY//oldcol/oldtbl}")" == "$want"
		sql legacy "VACUUM FULL oldtbl" >/dev/null || bail '3.1 table rewrite'
		check 'VACUUM FULL upgrades 3.1 groups to minor 6' test "$(sql legacy "SELECT min(format) FROM sift.table_segments('oldtbl') WHERE kind = 'columns'")" == 6
		check 'rewritten 3.1 groups answer null predicates exactly' test "$(sql legacy "SET seq_page_cost = 10000" "${QUERY//oldcol/oldtbl}")" == "$want"
	fi
	check "$ref write-buffer null predicates equal the heap" test "$(sql legacy "SET seq_page_cost = 10000" "$QUERY")" == "$want"
	check "$ref write-buffer mixed astext kinds equal the heap" test "$(sql legacy "SET seq_page_cost = 10000" "$TYPES")" == "$types"
	sql legacy "SELECT sift.col_promote('oldcol_idx')" >/dev/null || bail 'promotion'
	check "$ref promotion preserves uncertain null kinds" test "$(sql legacy "SET seq_page_cost = 10000" "$QUERY")" == "$want"
	check "$ref promotion preserves uncertain astext kinds" test "$(sql legacy "SET seq_page_cost = 10000" "$TYPES")" == "$types"
	check "$ref promotion does not claim minor 6" test "$(sql legacy "SELECT max(format) < 6 FROM sift.col_segments('oldcol_idx') WHERE kind = 'immutable'")" == t
	sql legacy "REINDEX INDEX oldcol_idx" >/dev/null || bail 'reindex'
	check "$ref REINDEX writes minor 12 (the complete kinds, with the sections that are on by default)" test "$(sql legacy "SELECT min(format) FROM sift.col_segments('oldcol_idx') WHERE kind = 'immutable'")" == 12
	check "$ref rewritten null predicates equal the heap" test "$(sql legacy "SET seq_page_cost = 10000" "$QUERY")" == "$want"
	check "$ref rewritten astext kinds equal the heap" test "$(sql legacy "SET seq_page_cost = 10000" "$TYPES")" == "$types"
	if [[ $ref == v3.1.0 ]]; then
		# the small-set summary (minor 10, opt-in): under the setting 4 the new library writes what 3.1 reads, under the default the 3.1 library refuses it
		SETQ="SELECT count(*) || ':' || sum(id) FROM setlow WHERE k = 2919"
		sql legacy "CREATE TABLE setlow AS SELECT i AS id, i * 7919 % 5000 AS k FROM generate_series(1, 3000) i" "CREATE TABLE sethigh AS TABLE setlow" \
			"SET sift.col_format_minor = 4" "CREATE INDEX setlow_idx ON setlow USING sift_col (id, k) WITH (set_columns = 'k')" "RESET sift.col_format_minor" \
			"CREATE INDEX sethigh_idx ON sethigh USING sift_col (id, k) WITH (set_columns = 'k')" "VACUUM ANALYZE setlow" "VACUUM ANALYZE sethigh" >/dev/null || bail 'minor 10 setup'
		setwant=$(sql legacy "SET sift.enable_agg = off" "SET sift.enable_col = off" "$SETQ")
		check 'the setting 4 writes no summary (minor 4) and the default one does (minor 12, with the sections that are on by default)' test "$(sql legacy "SELECT (SELECT max(format) FROM sift.col_segments('setlow_idx') WHERE kind = 'immutable') || ':' || (SELECT max(format) FROM sift.col_segments('sethigh_idx') WHERE kind = 'immutable')")" == 4:12
		# the sorted run (minor 13, opt-in): the setting 4 writes no run, the default one writes what 3.1 refuses
		ORDQ="SELECT string_agg(id::text, ',' ORDER BY k, id) FROM (SELECT id, k FROM ordlow ORDER BY k, id LIMIT 50) x"
		sql legacy "CREATE TABLE ordlow AS SELECT i AS id, i * 7919 % 5000 AS k FROM generate_series(1, 3000) i" "CREATE TABLE ordhigh AS TABLE ordlow" \
			"SET sift.col_format_minor = 4" "CREATE INDEX ordlow_idx ON ordlow USING sift_col (id, k) WITH (order_columns = 'k')" "RESET sift.col_format_minor" \
			"CREATE INDEX ordhigh_idx ON ordhigh USING sift_col (id, k) WITH (order_columns = 'k')" "VACUUM ANALYZE ordlow" "VACUUM ANALYZE ordhigh" >/dev/null || bail 'minor 13 setup'
		ordwant=$(sql legacy "SET sift.enable_agg = off" "SET sift.enable_col = off" "$ORDQ")
		check 'the setting 4 writes no run and the default one does' test "$(sql legacy "SELECT (SELECT max(format) FROM sift.col_segments('ordlow_idx') WHERE kind = 'immutable') || ':' || (SELECT max(format) FROM sift.col_segments('ordhigh_idx') WHERE kind = 'immutable')")" == 4:13
		node_stop legacy
		use_root "$oldroot"
		node_start legacy || bail 'cannot restart 3.1'
		err=$(sql legacy "SET enable_seqscan = off" "SELECT sift.col_verify('oldcol_idx', false)" 2>&1)
		diag "3.1 refusal: $err"
		check '3.1 refuses a minor 12 index (complete kinds and the default sections) with a format error and REINDEX hint' test "$(grep -c 'format 1.12\|REINDEX' <<<"$err")" -ge 2
		err=$(sql legacy "SELECT count(*) FROM oldtbl" 2>&1)
		check '3.1 refuses minor 6 table groups' grep -q 'format 1.6' <<<"$err"
		check '3.1 reads an index written with set_columns under the setting 4' test "$(sql legacy "SELECT sift.col_verify('setlow_idx', true)" "SET sift.enable_agg = on" "$SETQ" | tail -n 1)" == "$setwant"
		err=$(sql legacy "SET enable_seqscan = off" "SELECT sift.col_verify('sethigh_idx', false)" 2>&1)
		check '3.1 refuses a minor 12 set summary index with a format error and REINDEX hint' test "$(grep -c 'format 1.12\|REINDEX' <<<"$err")" -ge 2
		check '3.1 reads an index written with order_columns under the setting 4' test "$(sql legacy "SELECT sift.col_verify('ordlow_idx', true)" "SET sift.enable_agg = on" "$ORDQ" | tail -n 1)" == "$ordwant"
		err=$(sql legacy "SET enable_seqscan = off" "SELECT sift.col_verify('ordhigh_idx', false)" 2>&1)
		check '3.1 refuses a minor 13 index with a format error and REINDEX hint' test "$(grep -c 'format 1.13\|REINDEX' <<<"$err")" -ge 2
	fi
	node_stop legacy
	ROOT=$newroot
done
echo "1..$NTEST"
finish
