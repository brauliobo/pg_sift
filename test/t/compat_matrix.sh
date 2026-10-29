#!/usr/bin/env bash
# The upgrade matrix: what the releases 3.0.2, 3.1.0 and 3.2.0 wrote (a BM25 index, sift_col indexes over plain columns and jsonb paths, a
# sift_table; promoted segments and unpromoted write-buffer records), taken over by this tree in the same cluster: ALTER EXTENSION UPDATE, answers
# through the indexes equal to the plain plan and to what the release answered, sift.verify and sift.col_verify clean, REINDEX, writes (insert,
# update, delete, promotion, merge, VACUUM), and then the way back: the release reads what this tree writes at the lowest column format minor
# (3), at the newest minor it reads (and at 4), and with the options that are on by default switched off; and refuses with a format error and the
# REINDEX hint, instead of a wrong answer, an index of every newer minor (7 cube_minmax, 8 calendar_columns, 9 rollup_dims, 10 key_columns (a summary), 12 superunit,
# 13 order_columns, 14 a stored dictionary, 15 key_columns (a key list or bitmap), 16 point_tid_columns (heap addresses in the point directory), 17 key_cut (units cut by a key), 18 order_columns at the default setting (the runs with their heads), 19 a NULL-heavy integer leaf (the exceptions of its units), 20 hll_columns (a sketch of a column in every unit)), an index created at the defaults with no option, and the sift_table of the calendar cells (8)
# and of the wide-row write buffer (11); an index with order_columns under the lowest setting writes no run and is read. The last release then gets the
# documented downgrade (docs/UPGRADING.md section 4) of all of it: the setting, the rewrite of what the preflight lists, and the release serves the lot.
# The older libraries are built from the tags by scripts/old-lib-check.sh (cached; PostgreSQL 18, two library roots).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots; none supports 19 yet)'; exit 0; fi
# release | text kind of the jsonb path (3.0 has no astext) | sift_table (3.1 on) | newest column minor it reads
# Every release tag of the 3.0 to 3.2 series, so that a new release joins the matrix with its tag; SIFT_COMPAT_EXTRA_REFS adds refs of a release in preparation
# (origin/release/3.2.3) and SIFT_COMPAT_REFS limits the run to the refs it lists (a change of the test)
MATRIX=()
for ref in ${SIFT_COMPAT_REFS:-$(git -C "$REPO" tag -l 'v3.*' --sort=v:refname) ${SIFT_COMPAT_EXTRA_REFS:-}}; do
	case $ref in
		*3.0.*) MATRIX+=("$ref text no 3") ;;
		*3.1.*) MATRIX+=("$ref astext yes 4") ;;
		*3.2.*) MATRIX+=("$ref astext yes 6") ;;
	esac
done
((${#MATRIX[@]})) || { echo '1..0 # SKIP no release tag of the 3.0 to 3.2 series in this clone'; exit 0; }
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
	build_old "$ref"; oldroot=$OLDROOT
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
			"VACUUM tbl_ts" "CREATE TABLE tbl_big (id int, v text) USING sift_table" "INSERT INTO tbl_big VALUES (1, repeat('b', 2000000))" >/dev/null || bail "$ref table setup"
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
	if [[ $hastbl == yes ]]; then
		# a row of 2 MB the release buffered, and beside it a row of 5 MB, which this tree writes to tail pages (the buffer that takes it is format 11)
		BIGQ="SELECT id, length(v), md5(v) FROM tbl_big ORDER BY id"
		bigwant=$(q "SELECT 1, 2000000, md5(repeat('b', 2000000))" "SELECT 2, 5000000, md5(repeat('a', 5000000))" | tr '\n' ' ')
		check "$ref: the row of 2 MB the release buffered is read" test "$(q "$BIGQ" | tr '\n' ' ')" == "$(sed 's/ 2|.*//' <<<"$bigwant") "
		q "SET sift.table_wide_rows = on" "INSERT INTO tbl_big VALUES (2, repeat('a', 5000000))" >/dev/null
		check "$ref: the wide row opens a buffer of format 11" test "$(q "SELECT count(*) FROM sift.table_segments('tbl_big') WHERE kind = 'sealed buffer' AND format = 11")" == 1
		check "$ref: both rows are read" test "$(q "$BIGQ" | tr '\n' ' ')" == "$bigwant"
		q "VACUUM tbl_big" >/dev/null
		check "$ref: both rows are read after the promotion into columns" test "$(q "$BIGQ" | tr '\n' ' ')" == "$bigwant"
	fi
	check "$ref: promotion and merge" q "SELECT sift.promote('docs_idx'), sift.col_promote('docs_col'), sift.col_promote('jt_col')" \
		"SELECT sift.merge('docs_idx', true), sift.col_merge('docs_col', true), sift.col_merge('jt_col', true)" "VACUUM docs, jt"
	[[ $hastbl == no ]] || check "$ref: VACUUM of the sift_table" q "VACUUM tbl, tbl_h" "SELECT sift.table_maintain('tbl')"
	check "$ref: verify after the writes" verify_all "sift.verify('docs_idx', true)" "sift.col_verify('docs_col', true)" "sift.col_verify('jt_col', true)"
	[[ $hastbl == no ]] || check "$ref: the sift_table is its heap twin after the writes" twin_same
	agree "$ref: answers after promotion, merge and VACUUM equal the plain plan" "$(ans IDX "" "${what}b")" "$(ans PLAIN_NEW "" "${what}b")"

	# ---- the way back: clones written by this tree. Read by the release: the lowest minor, the newest it reads (and 4), and an index at the defaults that
	# opts out of what is on by default. Refused: one clone for each newer minor, and an index at the defaults with no option
	q "DROP INDEX docs_idx" >/dev/null	# a BM25 segment has no setting for an older reader: the release refuses a newer format by design
	# the setting exists once a function of the library ran in the session
	lowest=$(q "SELECT count(*) FROM sift.col_segments('docs_col')" "SELECT min_val FROM pg_settings WHERE name = 'sift.col_format_minor'" | tail -1)
	check "$ref: the lowest column format minor is 3" test "$lowest" == 3
	# fmt minor: the setting, and the minor the segments are stamped with (the clone at the defaults has no setting)
	formats=("lo $lowest")
	[[ $newest == 3 ]] || formats+=("hi $newest")
	((newest > 4)) && formats+=("h4 4")
	((newest >= 4)) && formats+=("ke 4")
	formats+=("cm 7" "ca 8" "rd 9" "sc 10" "su 12" "rn 13" "dk 14" "kp 15" "pt 16" "kc 17" "hd 18" "ex 19" "hl 20" "df 12")
	declare -A OPT=([lo]=", order_columns = 'g, n'" [hi]=", order_columns = 'g, n'" [h4]=", order_columns = 'g, n'" [rn]=", order_columns = 'g, n'" [cm]=", cube_minmax = true"
		[ca]=", calendar_columns = 'at'" [pt]=", point_tid_columns = 'g'" [kc]=", key_cut = 'g, n'" [hd]=", order_columns = 'g, n'" [hl]=", hll_columns = 'n'" [dk]=", dict_lookup = on" [rd]=", rollup_dims = 'g, n'" [sc]=", key_columns = 's'" [kp]=", key_columns = 'g'" [ke]=", cube_minmax = false, superunit = 0")
	declare -A SRC=([ca]="*, timestamp '2026-01-01' + id * interval '1 minute' AS at" [ex]="*, CASE WHEN id % 500 = 0 THEN id END AS z" [dk]="*, 'k' || id AS k" [sc]="*, (id % 40) * 7 AS s")
	declare -A COLS=([ex]="id, g, n, z" [ca]="id, g, n, at" [dk]="id, g, n, k" [sc]="id, g, n, s")
	# the minor the data stamps beyond the setting: a key_columns leaf also gets the key table of a segment (22)
	declare -A STAMP=([kp]=22)
	docsonly=" h4 ke cm ca rd sc su rn dk kp pt kc hd ex hl df "
	refused=" cm ca rd sc su rn dk kp pt kc hd ex hl df "
	for f in "${formats[@]}"; do
		read -r fmt minor <<<"$f"
		set="SET sift.col_format_minor = $minor"; [[ $fmt != df && $fmt != pt && $fmt != kc && $fmt != hd && $fmt != ex && $fmt != hl ]] || set="SELECT 1"	# the option or the data stamps 16, 17, 18, 19 or 20 at the default setting (15)
		q "$set" \
			"CREATE TABLE docs_$fmt AS SELECT ${SRC[$fmt]:-*} FROM docs" "CREATE INDEX docs_${fmt}_col ON docs_$fmt USING sift_col (${COLS[$fmt]:-id, g, n}) WITH (write_buffer_limit = 2048${OPT[$fmt]:-})" \
			"INSERT INTO docs_$fmt (id, g, n, body) SELECT i, i % 9, i % 100, 'x' FROM generate_series(20001, 20400) i" "SELECT sift.col_promote('docs_${fmt}_col')" \
			"INSERT INTO docs_$fmt (id, g, n, body) SELECT i, i % 9, i % 100, 'y' FROM generate_series(20401, 20600) i" >/dev/null || bail "$ref $fmt docs"
		if [[ $refused == *" $fmt "* ]]; then
			check "$ref: the $fmt clone is stamped minor ${STAMP[$fmt]:-$minor} or later" test "$(q "SELECT max(format) >= ${STAMP[$fmt]:-$minor} FROM sift.col_segments('docs_${fmt}_col') WHERE kind = 'immutable'")" == t
		else
			same "$ref: the $fmt clone is written at a minor the release reads (at most $minor)" "$(q "SELECT max(format) <= $minor AND min(format) >= 3 FROM sift.col_segments('docs_${fmt}_col') WHERE kind = 'immutable'")" t
		fi
		[[ $docsonly == *" $fmt "* ]] && continue
		# a minor 3 segment stores no types: 3.1 refuses an astext leaf of it at query time with the REINDEX hint, by design
		kind=$textkind; [[ $fmt != lo ]] || kind=text
		q "SET sift.col_format_minor = $minor" \
			"CREATE TABLE jt_$fmt AS TABLE jt" "CREATE INDEX jt_${fmt}_col ON jt_$fmt USING sift_col (id, items, attr) WITH (paths = 'items[]{s text}, attr->a ${kind}')" \
			"INSERT INTO jt_$fmt SELECT i, '[{\"s\":\"gift\"}]', '{\"a\":1}' FROM generate_series(4001, 4200) i" >/dev/null || bail "$ref $fmt jt"
		if [[ $hastbl == yes ]]; then
			q "SET sift.col_format_minor = $minor" "CREATE TABLE tbl_$fmt (LIKE tbl) USING sift_table" "INSERT INTO tbl_$fmt SELECT * FROM tbl" "VACUUM (FREEZE) tbl_$fmt" \
				"INSERT INTO tbl_$fmt SELECT i, i % 9, i % 100 FROM generate_series(20001, 20300) i" >/dev/null || bail "$ref $fmt table"
		fi
	done
	# point_tid_columns (minor 16) and key_cut (minor 17): the option and the minor survive a merge, a refresh and a REINDEX of this tree, whatever the release that reads them later
	for opt in pt:point_tid_columns:16 kc:key_cut:17; do
		IFS=: read -r fmt option minor <<<"$opt"
		optfmt() { q "SELECT (max(format) >= $minor) || ':' || (SELECT coalesce(reloptions::text, '') ~ '$option=g' FROM pg_class WHERE relname = 'docs_${fmt}_col') FROM sift.col_segments('docs_${fmt}_col') WHERE kind = 'immutable'"; }
		optwant=$(q "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(id) FROM docs_$fmt WHERE g = 4")
		for step in "SELECT sift.col_merge('docs_${fmt}_col', true)" "SELECT sift.col_refresh('docs_${fmt}_col')" "REINDEX INDEX docs_${fmt}_col"; do
			check "$ref: the $fmt clone: ${step%%(*}" q "$step"
			same "$ref: the $fmt clone is minor $minor or later with its option after ${step%%(*}" "$(optfmt)" "true:true"
			check "$ref: the $fmt clone verifies after ${step%%(*}" q "SELECT sift.col_verify('docs_${fmt}_col', true)"
			same "$ref: the $fmt clone answers a lookup as the plain plan after ${step%%(*}" "$(q "SET sift.enable_col_scan = on" "SET enable_seqscan = off" "SELECT count(*), sum(id) FROM docs_$fmt WHERE g = 4")" "$optwant"
		done
	done
	check "$ref: the clone with order_columns under the lowest setting holds no run" test "$(q "SELECT count(*) FROM sift.col_sections('docs_lo_col') WHERE section = 'runs'")" == 0
	if [[ $hastbl == yes ]]; then
		# the two sift_table formats that no index shows: the calendar cells of a timestamp column (8) and the write buffer of a row on tail pages (11)
		q "CREATE TABLE tbl_ca (id int, at timestamp) USING sift_table" "INSERT INTO tbl_ca SELECT i, timestamp '2026-01-01' + i * interval '1 hour' FROM generate_series(1, 3000) i" "VACUUM tbl_ca" \
			"SET sift.table_wide_rows = on" "CREATE TABLE tbl_wr (id int, v text) USING sift_table" "INSERT INTO tbl_wr VALUES (1, repeat('a', 5000000)), (2, 'short')" >/dev/null || bail "$ref table clones"
		check "$ref: tbl_ca holds column format 8" test "$(q "SELECT string_agg(DISTINCT format::text, ',') FROM sift.table_segments('tbl_ca') WHERE kind = 'columns'")" == 8
		check "$ref: tbl_wr holds a buffer of format 11" test "$(q "SELECT count(*) FROM sift.table_segments('tbl_wr') WHERE kind = 'sealed buffer' AND format = 11")" == 1
	fi
	WRQ="SELECT count(*), sum(length(t::text)) FROM tbl_wr t"
	[[ $hastbl == no ]] || wrwant=$(q "$WRQ")
	declare -A want=()
	for f in "${formats[@]}"; do
		fmt=${f%% *}
		w=$what; [[ $docsonly != *" $fmt "* ]] || w=d; w=${w//b/}
		want[$fmt]=$(ans PLAIN_NEW "_$fmt" "$w")
		agree "$ref: $fmt clone, this tree: index answers equal the plain plan" "$(ans IDX "_$fmt" "$w")" "${want[$fmt]}"
		check "$ref: $fmt clone, this tree: col_verify" q "SELECT sift.col_verify('docs_${fmt}_col', true)"
	done
	node_stop mx

	# ---- the release reads them
	use_root mx "$oldroot"
	node_start mx || bail "cannot restart $ref"
	for f in "${formats[@]}"; do
		read -r fmt minor <<<"$f"
		w=$what; [[ $docsonly != *" $fmt "* ]] || w=d; w=${w//b/}
		if [[ $refused != *" $fmt "* ]]; then
			check "$ref reads the $fmt clone: col_verify" q "SELECT sift.col_verify('docs_${fmt}_col', true)"
			[[ $docsonly == *" $fmt "* ]] || check "$ref reads the $fmt clone: col_verify of the JSON paths index" q "SELECT sift.col_verify('jt_${fmt}_col', true)"
			agree "$ref reads the $fmt clone: index answers equal what this tree answered" "$(ans IDX "_$fmt" "$w")" "${want[$fmt]}" tolerate
			agree "$ref reads the $fmt clone: plain answers equal what this tree answered" "$(ans PLAIN "_$fmt" "$w")" "${want[$fmt]}"
		else
			err=$(q "SELECT sift.col_verify('docs_${fmt}_col', true)" 2>&1)
			check "$ref refuses the $fmt clone (minor $minor) with a format error and the REINDEX hint" test "$(grep -ci 'format\|REINDEX' <<<"$err")" -ge 2
			out=$(ans IDX "_$fmt" d)
			check "$ref never answers wrongly from the $fmt clone (the rows or a format error)" test "$out" == "${want[$fmt]}" -o "$(grep -ci 'format' <<<"$out")" -gt 0
			check "$ref still serves after the refusal" test "$(q "SELECT 1")" == 1
		fi
	done
	if [[ $hastbl == yes ]]; then
		err=$(q "SELECT count(*), sum(length(t::text)) FROM tbl_ca t" 2>&1)
		check "$ref refuses the sift_table tbl_ca (calendar cells, column format 8) with a format error and the hint" test "$(grep -ci 'format\|REINDEX' <<<"$err")" -ge 2
		# the buffer of a wide row (format 11) is a marker the release does not look at: it reads the rows, takes a write beside them and promotes the buffer
		check "$ref reads the rows of tbl_wr, the buffer of format 11, as this tree does" test "$(q "$WRQ")" == "$wrwant"
		check "$ref writes to tbl_wr and promotes its buffers" q "INSERT INTO tbl_wr VALUES (3, 'w')" "VACUUM tbl_wr"
		check "$ref reads the wide row intact after the promotion" test "$(q "SELECT id, length(v), md5(v) FROM tbl_wr ORDER BY id" | tr '\n' ' ')" == "1|5000000|$(q "SELECT md5(repeat('a', 5000000))") 2|5|$(q "SELECT md5('short')") 3|1|$(q "SELECT md5('w')") "
		# tbl itself was promoted by this tree's VACUUM, at the minor it writes now (the cube minimum and maximum, 7), which the release refuses too
		check "$ref still serves after the refusals of the tables" test "$(q "SELECT count(*) FROM tbl_h")" -gt 0
	fi
	node_stop mx

	# ---- the downgrade of everything above, as docs/UPGRADING.md section 4 says: preflight, the setting, the rewrite of what the preflight lists, preflight again,
	# then the release's library serves the database (its extension stays at the version of this tree: there is no script back)
	if [[ $ref == v3.2.0 ]]; then
		uc() { PGHOST=$(dir mx)/sock PGPORT=$(port mx) PGUSER=postgres "$REPO/scripts/upgrade-check.sh" "$@"; }
		use_root mx "$newroot"
		node_start mx || bail "cannot restart this tree for the downgrade"
		q "CREATE TABLE dgb AS SELECT id, body FROM docs" "CREATE INDEX dgb_idx ON dgb USING sift (body)" "INSERT INTO dgb SELECT i, 'alpha' || i % 5 FROM generate_series(30001, 30500) i" \
			"SET sift.table_wide_rows = on" "CREATE TABLE tbl_wr2 (id int, v text) USING sift_table" "INSERT INTO tbl_wr2 VALUES (1, repeat('a', 5000000)), (2, 'short')" >/dev/null || bail "downgrade setup"
		DGB="SELECT count(*), sum(id) FROM dgb WHERE body @@> 'alpha3 beta4'"
		bidx=$(q "SET enable_seqscan = off" "$DGB" | tail -1)
		blockers=$(uc --to 3.2 postgres | awk '$1 == "blocker"')
		check "3.2 downgrade: the preflight lists the objects the release cannot read" test "$(grep -c . <<<"$blockers")" -ge 12
		check "3.2 downgrade: it lists the clones of every newer minor and the sift_tables" test "$(grep -cE ' (docs_(cm|ca|rd|sc|su|rn|dk|kp|pt|kc|df)_col|tbl_ca|tbl_wr2) ' <<<"$blockers")" -ge 12
		check "3.2 downgrade: it lists no BM25 index" test "$(grep -c ' dgb_idx \| docs_idx ' <<<"$blockers")" == 0
		check "3.2 downgrade: it lists the two event triggers whose function the release does not have" test "$(grep -cE ' sift_table_(ddl_check|repack_guard) +trigger ' <<<"$blockers")" == 2
		q "ALTER SYSTEM SET sift.col_format_minor = 6" "SELECT pg_reload_conf()" >/dev/null
		for _ in $(seq 100); do [[ $(q "SHOW sift.col_format_minor") == 6 ]] && break; sleep 0.1; done
		check "3.2 downgrade: the setting is 6 for every new session" test "$(q "SHOW sift.col_format_minor")" == 6
		while read -r _ obj kind _; do
			case $kind in
				index) q "REINDEX INDEX CONCURRENTLY $obj" >/dev/null || diag "REINDEX $obj failed" ;;
				table) q "VACUUM FULL $obj" >/dev/null || diag "VACUUM FULL $obj failed" ;;
				trigger) q "ALTER EVENT TRIGGER $obj DISABLE" >/dev/null || diag "ALTER EVENT TRIGGER $obj failed" ;;
			esac
		done <<<"$blockers"
		uc --to 3.2 postgres >/dev/null; rc=$?
		check "3.2 downgrade: the preflight has no blocker left" test $rc -eq 0
		left=$(q "WITH i AS MATERIALIZED (SELECT c.oid, c.relname, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift_col', 'sift_table') AND c.relkind IN ('i', 'r'))
			SELECT relname || ':' || (SELECT max(format) FROM sift.col_segments(oid::regclass)) FROM i WHERE amname = 'sift_col' AND (SELECT max(format) FROM sift.col_segments(oid::regclass)) > 6
			UNION ALL SELECT relname || ':' || (SELECT max(format) FROM sift.table_segments(oid::regclass)) FROM i WHERE amname = 'sift_table' AND (SELECT max(format) FROM sift.table_segments(oid::regclass)) > 6" | tr '\n' ' ')
		check "3.2 downgrade: no segment is above minor 6 or in a buffer of format 11" test -z "$left"
		[[ -z $left ]] || diag "left above 6: $left"
		node_stop mx
		use_root mx "$oldroot"
		node_start mx || bail "cannot restart $ref after the downgrade"
		check "3.2 downgrade: the release's library is the one that runs" test "$(q "SELECT sift.version()" | cut -c1-3)" == 3.2
		for idx in $(q "SELECT c.oid::regclass FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col' AND c.relkind = 'i' ORDER BY 1"); do
			check "3.2 downgrade: $idx verifies" q "SELECT sift.col_verify('$idx', true)"
		done
		check "3.2 downgrade: the BM25 index verifies" q "SELECT sift.verify('dgb_idx', true)"
		check "3.2 downgrade: the BM25 index answers as before" test "$(q "SET enable_seqscan = off" "$DGB" | tail -1)" == "$bidx"
		for f in "${formats[@]}"; do
			fmt=${f%% *}
			w=$what; [[ $docsonly != *" $fmt "* ]] || w=d; w=${w//b/}
			agree "3.2 downgrade: the $fmt clone answers as this tree answered" "$(ans IDX "_$fmt" "$w")" "${want[$fmt]}" tolerate
		done
		check "3.2 downgrade: tbl_ca and tbl_wr2 (the wide row) are read" test "$(q "SELECT count(*) FROM tbl_ca" "SELECT id, length(v) FROM tbl_wr2 ORDER BY id" | tr '\n' ' ')" == "3000 1|5000000 2|5 "
		check "3.2 downgrade: tbl_big (the wide row) is read" test "$(q "$BIGQ" | tr '\n' ' ')" == "$bigwant"
		check "3.2 downgrade: the release writes, promotes, merges and rewrites" q "INSERT INTO docs_df SELECT i, i % 9, i % 100, 'z' FROM generate_series(30001, 30400) i" \
			"SELECT sift.col_promote('docs_df_col')" "SELECT sift.col_merge('docs_df_col', true)" "REINDEX INDEX docs_df_col" "INSERT INTO tbl_ca VALUES (3001, now())" "VACUUM tbl_ca" \
			"INSERT INTO dgb VALUES (30501, 'alpha3 beta4')" "SELECT sift.promote('dgb_idx')"
		check "3.2 downgrade: and the indexes verify after the writes" verify_all "sift.col_verify('docs_df_col', true)" "sift.verify('dgb_idx', true)"
		check "3.2 downgrade: ALTER TABLE and CREATE TRIGGER work with the event triggers disabled" q "ALTER TABLE dgb ADD COLUMN extra int" \
			"CREATE TRIGGER t_dg BEFORE UPDATE ON dgb FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger()" "DROP TRIGGER t_dg ON dgb"
		err=$(q "ALTER EXTENSION pg_sift UPDATE TO '3.2'" 2>&1)
		check "3.2 downgrade: there is no script back" grep -q 'no update path' <<<"$err"
		check "3.2 downgrade: the catalog stays at 3.6" test "$(q "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == 3.6
		# pg_dump writes CREATE EXTENSION without a version: a server that has the 3.2 files only restores the database at 3.2, and every index with an option of
		# 3.3 fails to be created (the tables, the other indexes and the data come through)
		qd() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir mx)/sock" -p "$(port mx)" -U postgres -d "$db" "${@/#/--command=}"; }
		PGHOST=$(dir mx)/sock PGPORT=$(port mx) PGUSER=postgres pg_dump -d postgres -f "$(dir mx)/dump.sql" || bail "pg_dump failed"
		q "CREATE DATABASE rst" >/dev/null || bail "CREATE DATABASE rst"
		errs=$(PGHOST=$(dir mx)/sock PGPORT=$(port mx) PGUSER=postgres psql -X -q -d rst -f "$(dir mx)/dump.sql" 2>&1 >/dev/null | grep ERROR)
		# the indexes whose dumped options are not 3.2's: the ones with an OPT, and every sift_col index whose build chose a rollup of the rows (rollup_dims = 'auto: a, b', the default of 3.5)
		pat=$(for o in "${OPT[@]}"; do sed "s/^, *//; s/ *=.*//" <<<"$o"; done | sort -u | paste -sd'|' -); pat="$pat|rollup_dims"
		withopt=$(grep -cE "^CREATE INDEX .* USING sift_col .*WITH \(.*($pat)" "$(dir mx)/dump.sql")
		check "3.2 downgrade: the restore into the 3.2 library fails for the $withopt indexes with an option of 3.3" test "$(grep -c 'unrecognized parameter' <<<"$errs")" == "$withopt"
		check "3.2 downgrade: and for the 3 sift_tables with a row over a write buffer page, which load no row, and nothing else" test "$(grep -c 'is too large' <<<"$errs")/$(grep -c . <<<"$errs")" == "3/$((withopt + 3))"
		[[ $(grep -c . <<<"$errs") == $((withopt + 3)) ]] || diag "restore errors: $errs"
		check "3.2 downgrade: those tables are empty in the restored database" test "$(qd rst "SELECT (SELECT count(*) FROM tbl_big) + (SELECT count(*) FROM tbl_wr) + (SELECT count(*) FROM tbl_wr2)")" == 0
		check "3.2 downgrade: the restored database is at 3.2 with all its rows" test "$(qd rst "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'" "SELECT count(*) FROM docs_df" "SELECT count(*) FROM tbl_ca" | tr '\n' ' ')" == \
			"3.2 $(q "SELECT count(*) FROM docs_df") $(q "SELECT count(*) FROM tbl_ca") "
		check "3.2 downgrade: the restored indexes without such an option verify" qd rst "SELECT sift.col_verify(c.oid::regclass::text, true) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col'" "SELECT sift.verify('dgb_idx', true)"
		node_stop mx
		# and forward again: the 3.6 library on the same files, the event triggers enabled, the setting reset
		use_root mx "$newroot"
		node_start mx || bail "cannot restart this tree after the downgrade"
		check "3.2 downgrade: this tree's library runs again" test "$(q "SELECT sift.version()" | cut -c1-3)" == 3.6
		q "ALTER EVENT TRIGGER sift_table_ddl_check ENABLE" "ALTER EVENT TRIGGER sift_table_repack_guard ENABLE" "ALTER SYSTEM RESET sift.col_format_minor" "SELECT pg_reload_conf()" >/dev/null
		check "3.2 downgrade: forward again: the event triggers are enabled" test "$(q "SELECT string_agg(evtenabled::text, '') FROM pg_event_trigger WHERE evtname IN ('sift_table_ddl_check', 'sift_table_repack_guard')")" == OO
		check "3.2 downgrade: forward again: ALTER TABLE works with this tree's library" q "ALTER TABLE dgb DROP COLUMN extra"
		check "3.2 downgrade: forward again: the indexes verify" verify_all "sift.col_verify('docs_df_col', true)" "sift.verify('dgb_idx', true)"
		check "3.2 downgrade: forward again: the restored database goes to 3.6 with ALTER EXTENSION UPDATE" test "$(qd rst "ALTER EXTENSION pg_sift UPDATE" "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == 3.6
		uc --to 3.6 postgres >/dev/null; rc=$?
		check "3.2 downgrade: forward again: the preflight finds nothing to block" test $rc -eq 0
		node_stop mx
	fi
	ROOT=$newroot
done
# an aborted loop must not pass: every release runs more than 30 checks
check "every release of the matrix ran to its end" test "$NTEST" -gt $((30 * ${#MATRIX[@]}))
echo "1..$NTEST"
finish
