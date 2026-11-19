# Shared by upgrade_compat.sh and upgrade_compat_standby.sh (sourced after lib.bash): the zoo of objects one library writes and another opens, and the
# checks of one object kind (it verifies with sift.check and answers as the heap).
CONF="autovacuum = off
sift.async_maintenance = off
max_prepared_transactions = 8
sift.on_corruption = quarantine
wal_level = replica"
# the setting of the release that this tree removed (docs/UPGRADING.md, "What 5.1 removes"): a 5.0 library refuses a scope whose member is in more than 3 keys, a 5.1 library warns of it in every session
RELEASE_CONF="sift.scope_max_multiplicity = 64"
# compat_root NODE ROOT: use_root, with the setting of the release on the nodes that load the release and not on the ones that load this tree
compat_root() { sed_i '/^sift\.scope_max_multiplicity = /d' "$(dir "$1")/data/postgresql.conf"; use_root "$1" "$2"; [[ $2 == "$newroot" ]] || node_conf "$1" "$RELEASE_CONF"; }

# KIND|INDEX|QUERY through the sift paths|QUERY through the heap (empty: the same text with every sift path off)
OBJECTS=(
	"bm25|docs_idx|SELECT count(*), sum(n) FROM docs WHERE body @@> 'alpha3 beta4'"
	"col|docs_col|SELECT g, count(*), sum(n) FROM docs GROUP BY g ORDER BY g"
	"key-cut|kc_cut|SELECT k, count(*), sum(v) FROM kc GROUP BY k ORDER BY k"
	"key-auto|kc_auto|SELECT k, sum(v) FROM kc GROUP BY k ORDER BY k"
	"partitioned|pt_col|SELECT g, count(*), sum(n) FROM pt GROUP BY g ORDER BY g"
	"scope|events_col|SELECT count(*), sum(v), min(id) FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 5)"
	"connect|fact_col|SELECT d.id % 7, count(*), sum(f.v) FROM fact f JOIN dim d ON d.id = f.dim_id GROUP BY 1 ORDER BY 1"
	"track-ctid|trk_c_col|SELECT g, count(*), sum(v) FROM trk_c GROUP BY g ORDER BY g"
	"track-id|trk_i_col|SELECT g, count(*), sum(v) FROM trk_i GROUP BY g ORDER BY g"
	"sift_table|tbl|SELECT g, count(*), sum(n) FROM tbl GROUP BY g ORDER BY g"
	"value-store|vs_1|SELECT n, count(*), count(DISTINCT a) FROM vs GROUP BY n ORDER BY n"
	"value-store-2|vs_2|SELECT b, count(*) FROM vs WHERE n < 5 GROUP BY b ORDER BY b LIMIT 20"
)
COLIDX="'docs_col','kc_cut','kc_auto','events_col','fact_col','dim_col','trk_c_col','trk_i_col','vs_1','vs_2'"

D=$TAPDIR/compat_snap
trap '[[ $NFAIL -ne 0 ]] || rm -rf "$D"; cleanup' EXIT
ON=("SET sift.enable_agg = on" "SET seq_page_cost = 10000")
OFF=("SET sift.enable_agg = off" "SET sift.enable_scan = off" "SET sift.enable_col_scan = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off")
answer() { sql "$1" "${@:2}" 2>&1 | tr '\n' ' '; }
# agree NODE INDEX QUERY [HEAPQUERY]: the answer through the sift paths is the heap's
agree() { local n=$1 want got; want=$(answer "$n" "${OFF[@]}" "${4:-$3}"); got=$(answer "$n" "${ON[@]}" "$3"); out="sift: $got / heap: $want"; [[ $got == "$want" && $got != *ERROR* && -n $got ]]; }
sound() { local n=$1; shift; out=$(sql "$n" "SELECT c.index::text || ' ' || c.problem || ' ' || coalesce(c.detail, '') FROM unnest(coalesce((SELECT array_agg(inhrelid::regclass) FROM pg_inherits WHERE inhparent = $1::regclass), ARRAY[$1::regclass])) u, LATERAL sift.check(u) c" 2>&1); [[ -z $out ]]; }
sound_all() { out=$(sql "$1" "SELECT index::text || ' ' || problem FROM sift.check() WHERE index::text <> ALL ('{${2:-}}')" 2>&1); [[ -z $out ]]; }
# refused NODE QUERY: the library cannot read what a newer one wrote and says so with the format error and the hint, not with damage or an answer
refused() { out=$(answer "$1" "${ON[@]}" "$2"); [[ $out =~ ERROR.*this\ build.*reads && $out == *REINDEX* && $out != *corrupted* ]]; }
# kinds in REFUSED_KINDS (space separated) are the ones the library must refuse; every other kind verifies and answers as the heap
compat() { # DIRECTION VARIANT NODE: one COMPAT line and one check for each object kind
	local dir=$1 var=$2 n=$3 o kind ix q p ok
	for o in "${OBJECTS[@]}"; do
		IFS='|' read -r kind ix q p <<<"$o"
		ok=FAIL
		if [[ " ${REFUSED_KINDS:-} " == *" $kind "* ]]; then
			refused "$n" "$q" && ok=PASS
			echo "# COMPAT $kind $dir $var refused $ok"
			check "$dir/$var: $kind ($ix) is refused with the format error" test "$ok" == PASS
			continue
		fi
		{ sound "$n" "'$ix'" && agree "$n" "$ix" "$q" "$p"; } && ok=PASS
		echo "# COMPAT $kind $dir $var $ok"
		check "$dir/$var: $kind ($ix) verifies and answers as the heap" test "$ok" == PASS
	done
}
heap_counts() { sql "$1" "SELECT 'docs=' || count(*) FROM docs" "SELECT 'tbl=' || count(*) FROM tbl" "SELECT 'trk=' || count(*) FROM trk_c" "SELECT 'vs=' || count(*) FROM vs" | tr '\n' ' '; }


# zoo_create NODE: every index kind of the matrix on generic data
zoo_create() {
	sql "$1" "CREATE EXTENSION pg_sift" \
	"CREATE TABLE docs (id int PRIMARY KEY, g int, n int, body text)" \
	"INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX docs_idx ON docs USING sift (body)" "CREATE INDEX docs_col ON docs USING sift_col (id, g, n)" \
	"CREATE TABLE kc (id int PRIMARY KEY, k int, v int)" "INSERT INTO kc SELECT i, i % 20, i % 50 FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX kc_cut ON kc USING sift_col (id, k, v) WITH (key_cut = 'k')" "CREATE INDEX kc_auto ON kc USING sift_col (k, v)" \
	"CREATE TABLE pt (id int, g int, n int) PARTITION BY RANGE (id)" \
	"CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO ($((ROWS / 2)))" "CREATE TABLE pt2 PARTITION OF pt DEFAULT" \
	"INSERT INTO pt SELECT i, i % 9, i % 100 FROM generate_series(1, $ROWS) i" "CREATE INDEX pt_col ON pt USING sift_col (id, g, n)" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id))" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 400) m, generate_series(0, 5) j WHERE j < m % 7 ON CONFLICT DO NOTHING" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int)" \
	"INSERT INTO events SELECT i, 1 + (i * 7919) % 420, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10 FROM generate_series(1, $ROWS) i" \
	"SET sift.col_unit_rows = 100" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)', key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at')" \
	"RESET sift.col_unit_rows" \
	"CREATE TABLE dim (id int PRIMARY KEY, name text)" "INSERT INTO dim SELECT i, 'd' || i FROM generate_series(1, 50) i" "CREATE INDEX dim_col ON dim USING sift_col (id, name)" \
	"CREATE TABLE fact (id int PRIMARY KEY, dim_id int REFERENCES dim (id), v int)" "INSERT INTO fact SELECT i, 1 + i % 50, i % 30 FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX fact_col ON fact USING sift_col (id, dim_id, v)" \
	"CREATE TABLE trk_c (id int PRIMARY KEY, g int, v int)" "INSERT INTO trk_c SELECT i, i % 8, i % 60 FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX trk_c_col ON trk_c USING sift_col (id, g, v) WITH (track_changes = 'ctid')" \
	"CREATE TABLE trk_i (id int PRIMARY KEY, g int, v int)" "INSERT INTO trk_i SELECT i, i % 8, i % 60 FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX trk_i_col ON trk_i USING sift_col (id, g, v) WITH (track_changes = 'id')" \
	"CREATE TABLE tbl (id int, g int, n int) USING sift_table" "INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(1, $ROWS) i" "VACUUM tbl" \
	"CREATE TABLE vs (id int, a text, b text, n int)" \
	"INSERT INTO vs SELECT i, md5(i::text) || md5((i + 1)::text), 'b' || i % 13, i % 10 FROM generate_series(1, $ROWS) i" "ANALYZE vs" \
	"CREATE INDEX vs_1 ON vs USING sift_col (id, a, n)" "CREATE INDEX vs_2 ON vs USING sift_col (a, b, n)" >/dev/null
}
