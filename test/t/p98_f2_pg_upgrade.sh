#!/usr/bin/env bash
# pg_upgrade of a cluster that holds format major 2: a sift_table with a primary key, a B-tree, narrow UPDATEs (column deltas), DELETEs, a visibility map of frozen rows, a pre-aggregate and a sort
# projection and a TTL; a sift_table of wide values (extents of their own); a partitioned sift_table; and a heap table with a sift_col index (a projection) and a scope. Each table has a heap twin
# that takes the same statements. From the previous major to this one when its binaries are installed (SIFT_PG_OLD_BIN, or the PGDG or the cached server of the major before this one: PostgreSQL 17 to 18),
# with pg_sift built for that major from a copy of this tree; otherwise to the same major in another data directory. pg_upgrade --check, then the upgrade by copy and, from the same old cluster, the
# upgrade by link: each upgraded cluster answers the digests of the rows, the answers through the sift_col index and the SiftAgg scan as through the normal plan, the projections, the TTL and the
# visibility map as before, sift.check finds nothing, and the writes, VACUUM (FREEZE), the maintenance and REINDEX that follow keep every table equal to its twin.
. "$(dirname "$0")/lib.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/lib_upgrade.bash"

OLD_REF=
unset SIFT_BUILD_DIR SIFT_BUILD_ROOT   # the copy of the tree that build_old compiles for the old major links the objects of this directory when it is set: the library of the wrong major
NEWCFG=$(command -v pg_config)
OLDBIN=${SIFT_PG_OLD_BIN:-}
if [[ -z $OLDBIN ]]; then
	for c in "/usr/lib/postgresql/$((PG_MAJOR - 1))/bin" "/usr/pgsql-$((PG_MAJOR - 1))/bin" "${XDG_CACHE_HOME:-$HOME/.cache}/sift/pg/$((PG_MAJOR - 1))."*/bin; do
		[[ $c != *inject* && -x $c/pg_config ]] && { OLDBIN=$c; break; }
	done
fi
OLDCFG=${OLDBIN:+$OLDBIN/pg_config}
OLDCFG=${OLDCFG:-$NEWCFG}
OLD_MAJOR=$("$OLDCFG" --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
if ((OLD_MAJOR < $(sed -n 's/^PG_MIN_MAJOR *= *\([0-9]*\).*/\1/p' "$REPO/Makefile" | head -n 1))); then OLDCFG=$NEWCFG; OLD_MAJOR=$PG_MAJOR; fi

OV=$TAPDIR/overlay
trap 'pg_ctl -D "$(dir old)/data" -m immediate stop >/dev/null 2>&1; cleanup; ((NFAIL)) || rm -rf "$(dir old)" "$(dir new2)" "$OV" "$TAPDIR/old-src"' EXIT
rm -rf "$OV"
mkdir -p "$OV"
diag "pg_upgrade from PostgreSQL $OLD_MAJOR to $PG_MAJOR"

overlay "$NEWCFG" "$OV/new" "$ROOT"
if ((OLD_MAJOR == PG_MAJOR)); then
	overlay "$OLDCFG" "$OV/old" "$ROOT"
else
	build_old "$OLDCFG" "$OV/oldroot" || { (($? == 77)) && exit 0; tail -n 20 "$TAPDIR/old-build.log"; bail "pg_sift does not build for PostgreSQL $OLD_MAJOR"; }
	overlay "$OLDCFG" "$OV/old" "$OV/oldroot"
fi
plan 25
OLDB=$OV/old$("$OLDCFG" --bindir)
NEWB=$OV/new$("$NEWCFG" --bindir)
PATH=$NEWB:$PATH

PRELOAD="shared_preload_libraries = 'pg_sift'"
initdb_node old 2 "$OLDB"
node_conf old "$PRELOAD
$("$REPO/scripts/pgtest.sh" conf | grep -E '^(pg_)?sift\.')"
LD_PRELOAD=${SIFT_PRELOAD:-} "$OLDB/pg_ctl" -D "$(dir old)/data" -l "$(dir old)/postgres.log" -w -t 120 start >/dev/null 2>&1 || { start_failed old; bail "the old cluster does not start"; }

# the twins: the same columns, a heap table, the same statements; @T is the table of the statement and @H its twin
SETUP=$(cat <<SQL
CREATE EXTENSION pg_sift;
CREATE TABLE ev (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL, tag text, ts timestamp NOT NULL) USING sift_table;
CREATE TABLE ev_h (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL, tag text, ts timestamp NOT NULL);
CREATE TABLE fz (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL) USING sift_table;
CREATE TABLE fz_h (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL);
CREATE TABLE dl (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL) USING sift_table;
CREATE TABLE dl_h (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL);
CREATE TABLE wb (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL, w text NOT NULL) USING sift_table;
CREATE TABLE wb_h (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL, w text NOT NULL);
CREATE TABLE pt (id int, k int NOT NULL, v int NOT NULL, day date NOT NULL) PARTITION BY RANGE (day);
CREATE TABLE pt_a PARTITION OF pt FOR VALUES FROM ('2026-01-01') TO ('2026-02-01') USING sift_table;
CREATE TABLE pt_b PARTITION OF pt FOR VALUES FROM ('2026-02-01') TO ('2026-03-01') USING sift_table;
CREATE TABLE pt_h (id int, k int NOT NULL, v int NOT NULL, day date NOT NULL);
CREATE TABLE fc (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL);
CREATE TABLE fc_h (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL);
CREATE TABLE vs_pairs (dim int NOT NULL, tenant text NOT NULL, PRIMARY KEY (dim, tenant));
CREATE TABLE vs (id int PRIMARY KEY, dim int NOT NULL, amount int NOT NULL);
CREATE FUNCTION dg(t regclass) RETURNS text LANGUAGE plpgsql AS \$\$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(v), 0) || '':'' || md5(coalesce(string_agg(t::text, '';'' ORDER BY id), '''')) FROM %s t', t) INTO r;
	RETURN r;
END \$\$;
CREATE FUNCTION ans(t regclass) RETURNS text LANGUAGE plpgsql AS \$\$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(k || '':'' || c || '':'' || s, '','' ORDER BY k), '''') FROM (SELECT k, count(*) c, sum(v) s FROM %s GROUP BY k) x', t) INTO r;
	RETURN r;
END \$\$;
CREATE FUNCTION ans_on(t regclass) RETURNS text LANGUAGE sql SET sift.enable_agg = on SET cpu_tuple_cost = 0.5 RETURN ans(t);
CREATE FUNCTION ans_off(t regclass) RETURNS text LANGUAGE sql SET sift.enable_agg = off RETURN ans(t);
CREATE FUNCTION vsq() RETURNS text LANGUAGE sql RETURN (SELECT count(*) || ':' || sum(amount) FROM vs WHERE dim IN (SELECT dim FROM vs_pairs WHERE tenant = 'tenant-3'));
CREATE FUNCTION vsq_on() RETURNS text LANGUAGE sql SET sift.enable_agg = on SET cpu_tuple_cost = 0.5 RETURN vsq();
CREATE FUNCTION vsq_off() RETURNS text LANGUAGE sql SET sift.enable_agg = off RETURN vsq();
SQL
)
# twin NODE TEMPLATE TABLE TWIN: the statement of TEMPLATE (@T is the table) for the table and then for its twin
twin() { local node=$1 tpl=$2 t=$3 h=$4; q "$node" postgres "${tpl//@T/$t}" "${tpl//@T/$h}" >/dev/null; }
# workload NODE FROM TO: rows FROM..TO are inserted, and the rows of every table are updated (column deltas) and deleted
workload() {
	local n=$1 a=$2 b=$3
	twin "$n" "INSERT INTO @T SELECT g, g % 13, g % 100, 'e' || g % 7, timestamp '2026-01-01' + g * interval '1 minute' FROM generate_series($a, $b) g" ev ev_h
	twin "$n" "INSERT INTO @T SELECT g, g % 5, g, (SELECT string_agg(md5(g || ':' || i), '') FROM generate_series(1, 1000) i) FROM generate_series($((a / 100)), $((a / 100 + 39))) g ON CONFLICT DO NOTHING" wb wb_h
	twin "$n" "INSERT INTO @T SELECT g, g % 7, g % 50, date '2026-01-01' + g % 55 FROM generate_series($a, $b) g" pt pt_h
	twin "$n" "INSERT INTO @T SELECT g, g % 17, g % 1000 FROM generate_series($a, $b) g" fc fc_h
	twin "$n" "UPDATE @T SET v = v + 1 WHERE id % 10 = 0" ev ev_h
	twin "$n" "UPDATE @T SET v = v + 2, tag = tag || 'x' WHERE id % 31 = 0" ev ev_h
	twin "$n" "DELETE FROM @T WHERE id % 29 = 0" ev ev_h
	twin "$n" "UPDATE @T SET w = repeat('u', 30000) WHERE id % 7 = 0" wb wb_h
	twin "$n" "UPDATE @T SET v = v + 1 WHERE id % 11 = 0" pt pt_h
	twin "$n" "DELETE FROM @T WHERE id % 13 = 0" pt pt_h
	twin "$n" "UPDATE @T SET v = v + 1 WHERE id % 9 = 0" fc fc_h
	twin "$n" "DELETE FROM @T WHERE id % 23 = 0" fc fc_h
}
qs old postgres <<<"$SETUP" >/dev/null || bail "the old cluster cannot be set up"
q old postgres "INSERT INTO vs_pairs SELECT g, 'tenant-' || (g % 9) FROM generate_series(1, 300) g" "INSERT INTO vs SELECT g, 1 + g % 320, g % 100 FROM generate_series(1, 6000) g" \
	"CREATE INDEX vs_pairs_j ON vs_pairs USING sift_col (dim, tenant) WITH (track_changes = 'ctid')" "ANALYZE vs" \
	"CREATE INDEX vs_a ON vs USING sift_col (id, dim, amount) WITH (scope = 'tenant: dim -> vs_pairs(dim, tenant)')" >/dev/null || bail "the scope cannot be set up"
workload old 1 6000
q old postgres "CREATE INDEX ev_k ON ev (k)" "CREATE INDEX fc_idx ON fc USING sift_col (id, k, v)" \
	"SELECT sift.create_projection('ev', 'by_k', 'GROUP BY k MEASURES count(*), sum(v)')" "SELECT sift.create_projection('ev', 'by_v', 'ORDER BY v, id INCLUDE (k)')" \
	"SELECT sift.create_projection('fc_idx', 'fc_by_k', 'GROUP BY k MEASURES count(*), sum(v)')" "SELECT sift.set_ttl('ev', 'ts', interval '36500 days')" \
	"VACUUM (FREEZE) ev" "VACUUM (FREEZE) wb" "VACUUM pt" "VACUUM fc" >/dev/null || bail "the objects cannot be set up"
workload old 6001 7000
q old postgres "VACUUM (FREEZE) ev" >/dev/null
twin old "INSERT INTO @T SELECT g, g % 13, g % 100 FROM generate_series(1, 6000) g" fz fz_h
q old postgres "VACUUM (FREEZE) fz" >/dev/null
twin old "INSERT INTO @T SELECT g, g % 13, g % 100 FROM generate_series(1, 3000) g" dl dl_h
q old postgres "VACUUM (FREEZE) dl" >/dev/null
twin old "UPDATE @T SET v = v + 1 WHERE id % 10 = 0" dl dl_h

TABLES="ev fz dl wb pt fc"
digests() { local t out=; for t in $TABLES; do out+=$(q "$1" postgres "SELECT dg('$t')")" "; done; echo "$out"; }
twins() { local t; for t in $TABLES; do [[ $(q "$1" postgres "SELECT dg('$t') = dg('${t}_h')") == t ]] || { echo f; return; }; done; echo t; }
answers() { q "$1" postgres "SELECT ans_off('ev')" "SELECT ans_off('pt')" "SELECT ans_off('fc')" "SELECT vsq_off()"; }
agree() { q "$1" postgres "SELECT ans_on('ev') = ans_off('ev') AND ans_on('pt') = ans_off('pt') AND ans_on('fc') = ans_off('fc') AND vsq_on() = vsq_off()"; }
problems() { q "$1" postgres "SELECT count(*) FROM sift.check(NULL, true)"; }
PROJECTIONS="SELECT string_agg(relation::text || ':' || name || ':' || kind || ':' || state || ':' || spec, ';' ORDER BY relation::text, name) FROM sift.projection_list()"
TTL="SELECT string_agg(relid::regclass || ':' || col || ':' || retention, ';') FROM sift.ttl_policies"
VM="SELECT pg_relation_size('fz', 'vm')"

check "the old cluster holds each table equal to its twin" test "$(twins old)" == t
check "and verifies" test "$(problems old)" == 0
DIGESTS=$(digests old); ANSWERS=$(answers old); PROJ=$(q old postgres "$PROJECTIONS"); TTLS=$(q old postgres "$TTL"); VMSIZE=$(q old postgres "$VM"); VERSIONS=$(q old postgres "SELECT extversion || ' ' || sift.version() FROM pg_extension WHERE extname = 'pg_sift'")
check "the old cluster has the visibility map of a frozen table and a chain of column deltas the VACUUM did not fold" test "$VMSIZE" -gt 0 \
	-a "$(q old postgres "SELECT dead_rows FROM sift.space('dl')")" == 0
check "the old cluster has two projections of the table, one of the index and the TTL" test "$(grep -o ':ready:' <<<"$PROJ" | wc -l | tr -d ' ')" == 3 -a "$TTLS" == "ev:ts:36500 days"
"$OLDB/pg_ctl" -D "$(dir old)/data" -m fast -w stop >/dev/null 2>&1 || bail "the old cluster does not stop"

# upgrade MODE NODE OFFSET [pg_upgrade options]: a new cluster of this tree's server, the upgrade of the old data directory into it, and the start
upgrade() {
	local node=$2 offset=$3
	initdb_node "$node" "$offset" "$NEWB"
	NODES+=("$node")
	node_conf "$node" "$PRELOAD
$("$REPO/scripts/pgtest.sh" conf)"
	UPG=("$NEWB/pg_upgrade" -b "$OLDB" -B "$NEWB" -d "$(dir old)/data" -D "$(dir "$node")/data" -U postgres -p "$(port old)" -P "$(port "$node")" -s "$(dir "$node")/sock" "${@:4}")
}
run_upgrade() { (cd "$(dir "$1")" && LD_PRELOAD=${SIFT_PRELOAD:-} "${UPG[@]}" "${@:2}" >"$(dir "$1")/pg_upgrade${2:-}.log" 2>&1); }

# verify NODE MODE: the checks that hold after an upgrade, then the writes
verify() {
	local n=$1 m=$2
	check "$m: the extension and the library are the ones of before" test "$(q "$n" postgres "SELECT extversion || ' ' || sift.version() FROM pg_extension WHERE extname = 'pg_sift'")" == "$VERSIONS"
	check "$m: sift.check finds nothing on any table or index" test "$(problems "$n")" == 0
	check "$m: every table holds the rows it held, by the digest of each" test "$(digests "$n")" == "$DIGESTS"
	check "$m: the sift_col indexes, the SiftAgg scans and the projections answer as the normal plan and as before the upgrade" test "$(agree "$n")" == t \
		-a "$(answers "$n")" == "$ANSWERS"
	check "$m: the projections and the TTL are the ones of before, and the visibility map has the size it had" test "$(q "$n" postgres "$PROJECTIONS")" == "$PROJ" \
		-a "$(q "$n" postgres "$TTL")" == "$TTLS" -a "$(q "$n" postgres "$VM")" == "$VMSIZE"
	workload "$n" 7001 8000
	twin "$n" "UPDATE @T SET v = v + 3 WHERE id % 7 = 0" dl dl_h
	twin "$n" "UPDATE @T SET v = v + 1 WHERE id % 5 = 0" fz fz_h
	twin "$n" "DELETE FROM @T WHERE id % 19 = 0" fz fz_h
	check "$m: writes (column deltas, deletes, wide values, partitions, the sift_col table) keep every table equal to its twin" test "$(twins "$n")" == t
	q "$n" postgres "VACUUM (FREEZE) ev" "VACUUM (FREEZE) fz" "VACUUM (FREEZE) dl" "VACUUM wb" "VACUUM pt" "VACUUM fc" "SELECT count(*) FROM sift.maintain('ev')" "SELECT count(*) FROM sift.maintain('fc_idx')" >/dev/null
	check "$m: VACUUM (FREEZE) and the maintenance pass, and the tables are still their twins' and verify" test "$(twins "$n")" == t -a "$(problems "$n")" == 0
	q "$n" postgres "REINDEX INDEX fc_idx" "REINDEX INDEX vs_a" "REINDEX INDEX ev_k" >/dev/null
	check "$m: REINDEX of the sift_col indexes and the B-tree passes: the answers are the normal plan's and the tables verify" test "$(agree "$n")" == t -a "$(problems "$n")" == 0
	check "$m: a projection is dropped and made again after the upgrade" test "$(q "$n" postgres "SELECT sift.drop_projection('ev', 'by_k')" "SELECT sift.create_projection('ev', 'by_k', 'GROUP BY k MEASURES count(*), sum(v)')" "SELECT ans_on('ev') = ans_off('ev')" | tr '\n' ' ')" == "t t t "
}

# pg_upgrade --check changes nothing; the upgrade by copy follows, and then, from the same old data directory, the one by link
upgrade copy new 3 --copy
run_upgrade new --check
checked=$?
check "pg_upgrade --check passes" test $checked -eq 0
[[ $checked -eq 0 ]] || diag "$(tail -n 20 "$(dir new)/pg_upgrade--check.log"; cat "$(dir new)"/data/pg_upgrade_output.d/*/loadable_libraries.txt 2>/dev/null)"
run_upgrade new
upgraded=$?
check "pg_upgrade --copy succeeds" test $upgraded -eq 0
[[ $upgraded -eq 0 ]] || { upgrade_failed new; bail "pg_upgrade failed"; }
node_start new || bail "the cluster upgraded by copy does not start"
verify new copy

upgrade link new2 4 --link
run_upgrade new2
upgraded=$?
check "pg_upgrade --link succeeds" test $upgraded -eq 0
[[ $upgraded -eq 0 ]] || { upgrade_failed new2; bail "pg_upgrade --link failed"; }
node_start new2 || bail "the cluster upgraded by link does not start"
verify new2 link
finish
