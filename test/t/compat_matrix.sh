#!/usr/bin/env bash
# The upgrade matrix: what the releases 3.0.2, 3.1.0 and 3.2.0 wrote (a BM25 index, sift_col indexes over plain columns and jsonb paths, a
# sift_table; promoted segments and unpromoted write-buffer records), taken over by this tree in the same cluster: ALTER EXTENSION UPDATE, answers
# through the indexes equal to the plain plan and to what the release answered, sift.verify and sift.col_verify clean, REINDEX, writes (insert,
# update, delete, promotion, merge, VACUUM), and then the way back: the release reads what this tree writes at the lowest column format minor
# (3), at the newest minor it reads, and refuses an index of the minor 7 (cube_minmax), the minor 13 (order_columns) or the minor 14 (a stored dictionary) or the minor 15 (key_presence) with a format error instead of a wrong answer; an index with order_columns under the lowest setting writes no run and is read.
# The older libraries are built from the tags by scripts/old-lib-check.sh (cached; PostgreSQL 18, two library roots).
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots; none supports 19 yet)'; exit 0; fi
# release | text kind of the jsonb path (3.0 has no astext) | sift_table (3.1 on) | newest column minor it reads
MATRIX=("v3.0.2 text no 3" "v3.1.0 astext yes 4" "v3.2.0 astext yes 6")
for row in "${MATRIX[@]}"; do
	git -C "$REPO" rev-parse --verify -q "${row%% *}^{commit}" >/dev/null || { echo "1..0 # SKIP released library source ${row%% *} is unavailable"; exit 0; }
done
newroot=$ROOT
q() { sql mx "$@"; }

# the queries; the names of the tables are replaced by the clone of the format being read (docs -> docs_lo)
QD="SELECT g, count(*), sum(n), min(id), max(id) FROM docs GROUP BY g ORDER BY g"
QD2="SELECT count(*), sum(n) FROM docs WHERE n < 40 AND g <> 3"
QJ="SELECT count(*) FILTER (WHERE jsonb_path_exists(items, '\$[*] ? (@.s != \"gift\")')) || ':' ||
 count(*) FILTER (WHERE jsonb_path_exists(items, '\$[*] ? (@.s == null)')) || ':' ||
 count(*) FILTER (WHERE items @> '[{\"s\":null}]') FROM jt"
QJ2="SELECT string_agg(coalesce(t, 'missing') || ':' || n, ',' ORDER BY t NULLS FIRST)
 FROM (SELECT jsonb_typeof(attr->'a') t, count(*) n FROM jt GROUP BY 1) s"
QT="SELECT g, count(*), sum(n) FROM tbl GROUP BY g ORDER BY g"
QT2="SELECT count(*), sum(n), count(DISTINCT id) FROM tbl WHERE id % 7 = 3"
QB="SELECT count(*) FROM docs WHERE body @@> 'alpha3 beta4'"
QB2="SELECT count(*), sum(n) FROM docs WHERE body @@> 'beta7 NOT alpha1'"
TWIN="SELECT count(*), sum(id), sum(g * 3 + n) FROM"
IDX=("SET sift.enable_agg = on" "SET seq_page_cost = 10000")
PLAIN=("SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off")
PLAIN_NEW=("${PLAIN[@]}" "SET sift.enable_scan = off" "SET sift.enable_count = off")

# ans MODE SUFFIX WHAT: the answers of the queries WHAT (d docs, j jt, t tbl, b BM25 on docs) over the tables with the suffix, in plan mode idx or plain
ans() {
	local -n sets=$1
	local sfx=$2 what=$3 qs=() w
	for w in $(grep -o . <<<"$what"); do
		case $w in
			d) qs+=("$QD" "$QD2") ;;
			j) qs+=("$QJ" "$QJ2") ;;
			t) qs+=("$QT" "$QT2") ;;
			b) qs+=("$QB" "$QB2") ;;
		esac
	done
	local out=() x
	for x in "${qs[@]}"; do x=${x//FROM docs/FROM docs$sfx}; x=${x//FROM jt/FROM jt$sfx}; x=${x//FROM tbl/FROM tbl$sfx}; out+=("$x"); done
	for x in "${out[@]}"; do printf '%s\001' "$(q "${sets[@]}" "$x" 2>&1)"; done
}
verify_all() { # CALL...: the verifiers, each an SQL call
	local w
	for w in "$@"; do q "SELECT $w" >/dev/null || return 1; done
}
# agree DESCRIPTION GOT WANT [tolerate]: the answers query by query (ans separates them by ^A) are equal, or with tolerate a query of GOT is the REINDEX refusal
# of an index that does not store the types (a release before the kinds were complete, on a minor 3 segment: an error and no row, by design)
agree() {
	local i bad=0 g w
	mapfile -d $'\001' -t g < <(printf '%s' "$2")
	mapfile -d $'\001' -t w < <(printf '%s' "$3")
	[[ ${#g[@]} -eq ${#w[@]} ]] || bad=1
	for i in "${!w[@]}"; do
		[[ ${g[i]:-} == "${w[i]}" || (-n ${4:-} && ${g[i]:-} == *"REINDEX the index to store them"*) ]] || { bad=1; diag "query $i: got ${g[i]:0:300} / want ${w[i]:0:300}"; }
	done
	if [[ $bad -ne 0 ]]; then not_ok "$1"; else ok "$1"; fi
}
# same DESCRIPTION ACTUAL EXPECTED: an ok when equal, else the two in the diagnostics
same() { if [[ $2 == "$3" ]]; then ok "$1"; else not_ok "$1"; diag "got:  ${2:0:700}"; diag "want: ${3:0:700}"; fi; }
twin_same() { [[ $(q "$TWIN tbl") == "$(q "$TWIN tbl_h")" ]]; }

for row in "${MATRIX[@]}"; do
	read -r ref textkind hastbl newest <<<"$row"
	what=dj; [[ $hastbl == no ]] || what=djt
	oldroot=$(SIFT_OLD_REF=$ref "$REPO/scripts/old-lib-check.sh" build) || bail "cannot build $ref"
	node_init mx 2 'autovacuum = off
sift.async_maintenance = off'
	use_root mx "$oldroot"
	node_start mx || bail "cannot start $ref"

	# ---- the release writes
	q "CREATE EXTENSION pg_sift" \
		"CREATE TABLE docs (id int PRIMARY KEY, g int, n int, body text)" \
		"INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 12000) i" \
		"CREATE INDEX docs_idx ON docs USING sift (body)" "CREATE INDEX docs_col ON docs USING sift_col (id, g, n)" \
		"INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 3 || ' beta' || i % 7 FROM generate_series(12001, 12600) i" \
		"CREATE TABLE lw (id int, body text)" "INSERT INTO lw VALUES (1, repeat('x', 300))" "CREATE INDEX lw_idx ON lw USING sift (body)" \
		"CREATE TABLE jt (id int, items jsonb, attr jsonb)" \
		"INSERT INTO jt SELECT i, jsonb_build_array(CASE i % 3 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{\"s\":null}'::jsonb ELSE '{\"s\":\"gift\"}'::jsonb END),
		 CASE i % 5 WHEN 0 THEN '{}'::jsonb WHEN 1 THEN '{\"a\":null}'::jsonb WHEN 2 THEN '{\"a\":true}'::jsonb WHEN 3 THEN '{\"a\":1}'::jsonb ELSE '{\"a\":\"1\"}'::jsonb END
		 FROM generate_series(1, 3000) i" \
		"CREATE INDEX jt_col ON jt USING sift_col (id, items, attr) WITH (paths = 'items[]{s text}, attr->a $textkind')" \
		"INSERT INTO jt SELECT i, '[{\"s\":\"gift\"}]', '{\"a\":null}' FROM generate_series(3001, 3300) i" >/dev/null || bail "$ref setup"
	if [[ $hastbl == yes ]]; then
		q "CREATE TABLE tbl (id int, g int, n int) USING sift_table" "INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(1, 12000) i" "VACUUM tbl" \
			"INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(12001, 12600) i" "CREATE TABLE tbl_h AS TABLE tbl" \
			"CREATE TABLE tbl_ts (id int, at timestamp) USING sift_table" "INSERT INTO tbl_ts SELECT i, timestamp '2026-01-01' + i * interval '1 hour' FROM generate_series(1, 3000) i" \
			"VACUUM tbl_ts" >/dev/null || bail "$ref table setup"
	fi
	q "VACUUM docs, jt" >/dev/null
	a=$(ans IDX "" "${what}b"); plain=$(ans PLAIN "" "${what}b")
	check "$ref: its indexes answer as its plain plan" test "$a" == "$plain"
	check "$ref: it left promoted segments and unpromoted records" test "$(q "SELECT count(*) FILTER (WHERE kind = 'immutable') > 0 AND count(*) FILTER (WHERE kind <> 'immutable') > 0 FROM sift.col_segments('docs_col')")" == t
	node_stop mx

	# ---- this tree takes over
	use_root mx "$newroot"
	node_start mx || bail "cannot start this tree on $ref data"
	upd=$(LC_ALL=C q "ALTER EXTENSION pg_sift UPDATE" 2>&1); rc=$?
	check "$ref: ALTER EXTENSION UPDATE" test $rc -eq 0
	# the text index is always named (a word over 255 bytes), the JSON paths index of a release before column format 6 too, and a sift_table without calendar cells
	check "$ref: the update names the text indexes and the two column indexes (the sections that are on by default since 3.3 lack in every unit of that release)" test \
		"$(grep -c 'REINDEX INDEX CONCURRENTLY public.docs_idx;' <<<"$upd")/$(grep -c 'REINDEX INDEX CONCURRENTLY public' <<<"$upd")" == "1/4"
	check "$ref: the update names the VACUUM FULL of the timestamp sift_table when the release has one" test "$(grep -c 'VACUUM FULL public.tbl_ts;' <<<"$upd")/$(grep -c 'VACUUM FULL public' <<<"$upd")" == "$([[ $hastbl == yes ]] && echo 1/1 || echo 0/0)"
	check "$ref: the extension is at the version of this tree" test "$(q "SELECT extversion = default_version FROM pg_extension, pg_available_extensions WHERE extname = 'pg_sift' AND name = 'pg_sift'")" == t
	check "$ref: the functions added in 3.3 exist after the update" test "$(q "SELECT count(*) FROM pg_proc WHERE pronamespace = 'sift'::regnamespace
		AND proname IN ('col_refresh', 'col_clustering', 'stats', 'stats_reset', 'col_rollup_misses', 'explain_why', 'upgrade_commands')")" == 7
	agree "$ref: answers after the update equal what $ref answered" "$(ans IDX "" "${what}b")" "$a"
	agree "$ref: answers after the update equal the plain plan" "$(ans IDX "" "${what}b")" "$(ans PLAIN_NEW "" "${what}b")"
	check "$ref: sift.verify and sift.col_verify are clean" verify_all "sift.verify('docs_idx', true)" "sift.col_verify('docs_col', true)" "sift.col_verify('jt_col', true)"
	[[ $hastbl == no ]] || check "$ref: the sift_table is its heap twin" twin_same
	# the index of the release holds the word cut at 255 bytes: it finds nothing until the REINDEX the update printed
	LONGQ="SELECT count(*) FROM lw WHERE body @@> repeat('x', 300)"
	check "$ref: the index written by the release does not find a word of 300 bytes" test "$(q "SET enable_seqscan = off" "$LONGQ" | tail -n 1)" == 0
	check "$ref: REINDEX" q "REINDEX INDEX docs_idx" "REINDEX INDEX docs_col" "REINDEX INDEX jt_col" "REINDEX INDEX CONCURRENTLY public.lw_idx"
	check "$ref: after the REINDEX the index finds the word of 300 bytes" test "$(q "SET enable_seqscan = off" "$LONGQ" | tail -n 1)" == 1
	[[ $hastbl == no ]] || check "$ref: VACUUM FULL of the timestamp sift_table writes column format 8 and leaves the list" test "$(q "SELECT count(*) FROM tbl_ts" \
		"VACUUM FULL tbl_ts" "SELECT string_agg(DISTINCT format::text, ',') || ':' || (SELECT count(*) FROM sift.upgrade_commands() WHERE object = 'tbl_ts'::regclass) FROM sift.table_segments('tbl_ts') WHERE kind = 'columns'" | tr '\n' ' ')" == "3000 8:0 "
	check "$ref: verify after REINDEX" verify_all "sift.verify('docs_idx', true)" "sift.col_verify('docs_col', true)" "sift.col_verify('jt_col', true)"
	agree "$ref: answers after REINDEX equal the plain plan" "$(ans IDX "" "${what}b")" "$(ans PLAIN_NEW "" "${what}b")"
	# writes with this tree on what the release wrote
	q "INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 4 || ' beta' || i % 13 FROM generate_series(12601, 13500) i" \
		"UPDATE docs SET n = n + 1, body = body || ' moved' WHERE id % 17 = 0" "DELETE FROM docs WHERE id % 19 = 0" \
		"INSERT INTO jt SELECT i, '[{\"s\":null},{\"s\":\"x\"}]', '{\"a\":2}' FROM generate_series(3301, 3600) i" \
		"UPDATE jt SET attr = '{\"a\":true}' WHERE id % 23 = 0" "DELETE FROM jt WHERE id % 29 = 0" >/dev/null || bail "$ref writes"
	if [[ $hastbl == yes ]]; then
		for t in tbl tbl_h; do
			q "INSERT INTO $t SELECT i, i % 9, i % 100 FROM generate_series(12601, 13500) i" "UPDATE $t SET n = n + 1 WHERE id % 17 = 0" "DELETE FROM $t WHERE id % 19 = 0" >/dev/null || bail "$ref table writes"
		done
	fi
	agree "$ref: answers after writes equal the plain plan" "$(ans IDX "" "${what}b")" "$(ans PLAIN_NEW "" "${what}b")"
	check "$ref: promotion and merge" q "SELECT sift.promote('docs_idx'), sift.col_promote('docs_col'), sift.col_promote('jt_col')" \
		"SELECT sift.merge('docs_idx', true), sift.col_merge('docs_col', true), sift.col_merge('jt_col', true)" "VACUUM docs, jt"
	[[ $hastbl == no ]] || check "$ref: VACUUM of the sift_table" q "VACUUM tbl, tbl_h" "SELECT sift.table_maintain('tbl')"
	check "$ref: verify after the writes" verify_all "sift.verify('docs_idx', true)" "sift.col_verify('docs_col', true)" "sift.col_verify('jt_col', true)"
	[[ $hastbl == no ]] || check "$ref: the sift_table is its heap twin after the writes" twin_same
	agree "$ref: answers after promotion, merge and VACUUM equal the plain plan" "$(ans IDX "" "${what}b")" "$(ans PLAIN_NEW "" "${what}b")"

	# ---- the way back: clones written by this tree at the lowest minor, the newest one the release reads, and the cube minmax
	q "DROP INDEX docs_idx" >/dev/null	# a BM25 segment has no setting for an older reader: the release refuses a newer format by design
	# the setting exists once a function of the library ran in the session
	lowest=$(q "SELECT count(*) FROM sift.col_segments('docs_col')" "SELECT min_val FROM pg_settings WHERE name = 'sift.col_format_minor'" | tail -1)
	check "$ref: the lowest column format minor is 3" test "$lowest" == 3
	formats=("lo $lowest" "cm 7" "rn 13" "kp 15"); [[ $newest == 3 ]] || formats=("lo $lowest" "hi $newest" "cm 7" "rn 13" "kp 15")
	for f in "${formats[@]}"; do
		read -r fmt minor <<<"$f"
		opt=", order_columns = 'g, n'"; [[ $fmt != cm ]] || opt=", cube_minmax = true"; [[ $fmt != kp ]] || opt=", key_presence = 'g'"
		q "SET sift.col_format_minor = $minor" \
			"CREATE TABLE docs_$fmt AS TABLE docs" "CREATE INDEX docs_${fmt}_col ON docs_$fmt USING sift_col (id, g, n) WITH (write_buffer_limit = 2048$opt)" \
			"INSERT INTO docs_$fmt SELECT i, i % 9, i % 100, 'x' FROM generate_series(20001, 20400) i" "SELECT sift.col_promote('docs_${fmt}_col')" \
			"INSERT INTO docs_$fmt SELECT i, i % 9, i % 100, 'y' FROM generate_series(20401, 20600) i" >/dev/null || bail "$ref $fmt docs"
		[[ $fmt == cm || $fmt == rn || $fmt == kp ]] && continue
		# a minor 3 segment stores no types: 3.1 refuses an astext leaf of it at query time with the REINDEX hint, by design
		kind=$textkind; [[ $fmt != lo ]] || kind=text
		q "SET sift.col_format_minor = $minor" \
			"CREATE TABLE jt_$fmt AS TABLE jt" "CREATE INDEX jt_${fmt}_col ON jt_$fmt USING sift_col (id, items, attr) WITH (paths = 'items[]{s text}, attr->a ${kind}')" \
			"INSERT INTO jt_$fmt SELECT i, '[{\"s\":\"gift\"}]', '{\"a\":1}' FROM generate_series(4001, 4200) i" >/dev/null || bail "$ref $fmt jt"
		if [[ $hastbl == yes ]]; then
			q "SET sift.col_format_minor = $minor" "CREATE TABLE tbl_$fmt (LIKE tbl) USING sift_table" "INSERT INTO tbl_$fmt SELECT * FROM tbl" "VACUUM (FREEZE) tbl_$fmt" \
				"INSERT INTO tbl_$fmt SELECT i, i % 9, i % 100 FROM generate_series(20001, 20300) i" >/dev/null || bail "$ref $fmt table"
		fi
		same "$ref: the $fmt clone is written at a minor the release reads (at most $minor)" "$(q "SELECT max(format) <= $minor AND min(format) >= 3 FROM sift.col_segments('docs_${fmt}_col') WHERE kind = 'immutable'")" t
	done
	check "$ref: the cube_minmax clone is minor 7" test "$(q "SELECT min(format) FROM sift.col_segments('docs_cm_col') WHERE kind = 'immutable'")" == 7
	check "$ref: the order_columns clone is minor 13" test "$(q "SELECT min(format) FROM sift.col_segments('docs_rn_col') WHERE kind = 'immutable'")" == 13
	check "$ref: the key_presence clone is minor 15" test "$(q "SELECT min(format) FROM sift.col_segments('docs_kp_col') WHERE kind = 'immutable'")" == 15
	check "$ref: the clone with order_columns under the lowest setting holds no run" test "$(q "SELECT count(*) FROM sift.col_sections('docs_lo_col') WHERE section = 'runs'")" == 0
	declare -A want=()
	for f in "${formats[@]}"; do
		fmt=${f%% *}
		w=d; [[ $fmt == cm || $fmt == rn || $fmt == kp ]] || w=$what; w=${w//b/}
		want[$fmt]=$(ans PLAIN_NEW "_$fmt" "$w")
		agree "$ref: $fmt clone, this tree: index answers equal the plain plan" "$(ans IDX "_$fmt" "$w")" "${want[$fmt]}"
		check "$ref: $fmt clone, this tree: col_verify" q "SELECT sift.col_verify('docs_${fmt}_col', true)"
	done
	node_stop mx

	# ---- the release reads them
	use_root mx "$oldroot"
	node_start mx || bail "cannot restart $ref"
	for f in "${formats[@]}"; do
		fmt=${f%% *}
		w=d; [[ $fmt == cm || $fmt == rn || $fmt == kp ]] || w=$what; w=${w//b/}
		if [[ $fmt != cm && $fmt != rn && $fmt != kp ]]; then
			check "$ref reads the $fmt clone: col_verify" q "SELECT sift.col_verify('docs_${fmt}_col', true)" "SELECT sift.col_verify('jt_${fmt}_col', true)"
			agree "$ref reads the $fmt clone: index answers equal what this tree answered" "$(ans IDX "_$fmt" "$w")" "${want[$fmt]}" tolerate
			agree "$ref reads the $fmt clone: plain answers equal what this tree answered" "$(ans PLAIN "_$fmt" "$w")" "${want[$fmt]}"
		else
			minor=${f##* }
			err=$(q "SELECT sift.col_verify('docs_${fmt}_col', true)" 2>&1)
			check "$ref refuses the minor $minor index with a format error and the REINDEX hint" test "$(grep -ci 'format\|REINDEX' <<<"$err")" -ge 2
			out=$(ans IDX "_$fmt" d)
			check "$ref never answers wrongly from the minor $minor index (the rows or a format error)" test "$out" == "${want[$fmt]}" -o "$(grep -ci 'format' <<<"$out")" -gt 0
			check "$ref still serves after the refusal" test "$(q "SELECT 1")" == 1
		fi
	done
	node_stop mx
	ROOT=$newroot
done
# an aborted loop must not pass: every release runs more than 30 checks
check "every release of the matrix ran to its end" test "$NTEST" -gt $((30 * ${#MATRIX[@]}))
echo "1..$NTEST"
finish
