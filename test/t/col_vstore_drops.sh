#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Three sift_col indexes of one table share the value store of its uuid and text columns (docs/COL-VSTORE.md). The store never loses or changes a value while a member
# exists, and goes in the transaction of the last member: after each of these, on the primary and on a hot standby, every shape answers as the heap does, every remaining member
# verifies, and the store has the values it had.
#  1. plain steps: drop the first member, a new member adopts the store, DROP INDEX CONCURRENTLY, REINDEX (CONCURRENTLY), a key_order that differs and returns, VACUUM FULL and CLUSTER
#     of the table, VACUUM FULL of a store, a column type change, the drop of the last but one member, of the last one (the stores go) and a build after it (new stores)
#  2. races: a build that waits for the drop of the last member makes a fresh store; a drop of the last old member that waits for a build that adopted the store leaves the store;
#     DROP TABLE and DROP SCHEMA .. CASCADE take their stores and no other table's
#  3. a reader that compares the indexes with the heap all the while members are dropped concurrently, reindexed concurrently, and promoted and merged after rows with new strings
#  4. kill -9: an uncommitted drop, a committed drop, REINDEX and col_promote killed in the leader at a directory update
#  5. pg_dump and pg_restore (serially and in parallel): one store for each column, every member adopts it
# SIFT_VD_ROWS (60000, 20000 quick)
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_VD_ROWS 60000 20000)
UIDS=$((ROWS * 5 / 12))
URLS=$((ROWS / 2))
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off
sift.test_vstore_columns = 'uid,url'
sift.async_maintenance = off"
node_start primary || bail "primary start failed"
D=$(dir primary)

dbq() { local node=$1 db=$2; shift 2; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$node")/sock" -p "$(port "$node")" -U postgres -d "$db" "${@/#/--command=}"; }

sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE t (id int PRIMARY KEY, tenant int, uid uuid, url text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT i, i % 7, md5((i % $UIDS)::text)::uuid,
		CASE WHEN i % 29 = 0 THEN NULL ELSE 'https://example.com/p/' || md5((i % $URLS)::text) END FROM generate_series(1, $ROWS) i" \
	"ANALYZE t" >/dev/null || bail "setup failed"

# one digest of the answers of every shape, from the indexes (agg = true) or from the heap; the values of a store, by code
sql primary >/dev/null <<'SQL' || bail "functions failed"
CREATE FUNCTION vd_digest(agg boolean) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE q text; r record; acc text := '';
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	FOREACH q IN ARRAY ARRAY[
		$q$SELECT count(*), sum(id) FROM t WHERE url = 'https://example.com/p/' || md5('77')$q$,
		$q$SELECT count(*), sum(id) FROM t WHERE url IN ('https://example.com/p/' || md5('1'), 'https://example.com/p/' || md5('7999'), '', 'none')$q$,
		$q$SELECT count(*), sum(id) FROM t WHERE url > 'https://example.com/p/5' AND url < 'https://example.com/p/6'$q$,
		$q$SELECT count(*), sum(id) FROM t WHERE url IS NULL$q$,
		$q$SELECT count(*), sum(id) FROM t WHERE uid = md5('5')::uuid$q$,
		$q$SELECT count(*), sum(id) FROM t WHERE uid IN (md5('1')::uuid, md5('7999')::uuid, md5('x')::uuid)$q$,
		$q$SELECT count(*), count(DISTINCT url), min(url), max(url), count(DISTINCT uid) FROM t$q$,
		$q$SELECT md5(string_agg(g::text, ',' ORDER BY g)) FROM (SELECT count(*) || '/' || url AS g FROM t GROUP BY url) z$q$,
		$q$SELECT md5(string_agg(g::text, ',' ORDER BY g)) FROM (SELECT count(*) || '/' || uid AS g FROM t WHERE tenant = 2 GROUP BY uid) z$q$,
		$q$SELECT count(*), count(DISTINCT url), sum(id) FROM t WHERE url >= 'https://example.com/n/' AND url < 'https://example.com/o/'$q$
	] LOOP
		EXECUTE q INTO r;
		acc := acc || r::text || ';';
	END LOOP;
	RETURN md5(acc);
END $f$;
CREATE FUNCTION vd_prefix(store regclass, n int8) RETURNS text LANGUAGE sql AS $$
	SELECT md5(coalesce(string_agg(sift._test_vstore_value(store, c), ',' ORDER BY c), '')) FROM generate_series(0, n - 1) c $$;
CREATE FUNCTION vd_fp(store regclass) RETURNS text LANGUAGE sql AS $$
	SELECT n || ':' || vd_prefix(store, n) FROM (SELECT view_values AS n FROM sift._test_vstore_info(store)) i $$;
SQL
sql primary "CREATE INDEX t_a ON t USING sift_col (id, tenant, uid, url)" "CREATE INDEX t_b ON t USING sift_col (id, uid, url)" \
	"CREATE INDEX t_c ON t USING sift_col (uid, url, tenant)" >/dev/null || bail "build failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

# --- what a step leaves, as a user sees it
VERIFY="SELECT sift.col_verify(c.oid, true) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col' AND c.relkind = 'i'"
# equal answers [DB]: the standby caught up; the indexes answer as the heap on the primary, the standby as the primary; every member verifies on both nodes
equal_answers() {
	local db=${1:-postgres} pv sv vp vs
	out=
	catchup primary standby || { out="the standby did not catch up"; return 1; }
	pv=$(dbq primary "$db" "SELECT vd_digest(true) || '/' || vd_digest(false)" 2>&1)
	sv=$(dbq standby "$db" "SELECT vd_digest(true)" 2>&1)
	vp=$(dbq primary "$db" "$VERIFY" 2>&1)
	vs=$(dbq standby "$db" "$VERIFY" 2>&1)
	out="primary indexes/heap: $pv; standby indexes: $sv; verify primary: $vp; verify standby: $vs"
	[[ ${pv%/*} == "${pv#*/}" && $sv == "${pv#*/}" && -z $vp && -z $vs ]]
}
# the stores of the table and the members that use each, on a node
stores() { dbq "$1" "${2:-postgres}" "SELECT coalesce(string_agg(column_name || '=' || indexes_using, ' ' ORDER BY column_name), 'none')
	FROM sift.col_value_store_status WHERE table_name = 't'::regclass"; }
stores_are() { # WANT [DB]
	local p s
	catchup primary standby || return 1
	p=$(stores primary "${2:-postgres}" 2>&1) s=$(stores standby "${2:-postgres}" 2>&1)
	out="want $1; primary $p; standby $s"
	[[ $p == "$1" && $s == "$1" ]]
}
# the values of the stores by code, one fingerprint of each: the same as in $BASE, or $BASE's codes with later ones appended (prefix)
fp() { sql primary "SELECT coalesce(string_agg(column_name || ':' || vd_fp(store), ' ' ORDER BY column_name), 'none')
	FROM sift.col_value_store_status WHERE table_name = 't'::regclass"; }
values_same() { out="start $BASE; now $(fp)"; [[ $(fp) == "$BASE" ]]; }
values_kept() { # the codes of $BASE are still the same ones
	local e col n h
	out="start $BASE"
	for e in $BASE; do
		IFS=: read -r col n h <<<"$e"
		[[ $(sql primary "SELECT vd_prefix('t_${col}_sift_values', $n)") == "$h" ]] || { out+="; the first $n codes of $col changed"; return 1; }
	done
}
# dependencies that point to a relation that is gone, in either direction
dangling() { sql "$1" "SELECT count(*) FROM pg_depend d WHERE (d.refclassid = 'pg_class'::regclass AND d.refobjid >= 16384
	AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = d.refobjid))
	OR (d.classid = 'pg_class'::regclass AND d.objid >= 16384 AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = d.objid))"; }
store_oids() { sql primary "SELECT 't_uid_sift_values'::regclass::oid || '/' || 't_url_sift_values'::regclass::oid"; }
exists_on_both() { # INDEX WANT(t|f)
	catchup primary standby && [[ $(sql primary "SELECT to_regclass('$1') IS NOT NULL") == "$2" && $(sql standby "SELECT to_regclass('$1') IS NOT NULL") == "$2" ]]
}

BASE=$(fp)
check "built: the three members share one store for the uid and one for the url" test "$(stores primary)" == "uid=3 url=3"
served() {
	out=$(sql primary "SET sift.enable_agg = on" "SET sift.agg_force = on" "EXPLAIN SELECT count(*), sum(id) FROM t WHERE url = 'https://example.com/p/' || md5('77')")
	[[ $out == *SiftAgg* ]]
}
check "built: a shape on the shared url column is served by a member" served
check "built: equal answers" equal_answers

# --- 1. plain steps. label|statement|stores after it; '>' makes the stores new (the values are taken again), 'rule:' lets the rule decide in that session
STEPS=(
	'drop the first member (its build made the store)|DROP INDEX t_a|uid=2 url=2'
	'a member made after it adopts the store again|CREATE INDEX t_d ON t USING sift_col (id, uid, url)|uid=3 url=3'
	'drop a middle member concurrently|DROP INDEX CONCURRENTLY t_b|uid=2 url=2'
	'reindex a member|REINDEX INDEX t_c|uid=2 url=2'
	'reindex a member concurrently|REINDEX INDEX CONCURRENTLY t_d|uid=2 url=2'
	'its key_order changes|ALTER INDEX t_c SET (key_order = '"'tenant'"')|uid=2 url=2'
	'rule:reindex: another order, its own strings, the store is released|REINDEX INDEX t_c|uid=1 url=1'
	'the key_order is the same again|ALTER INDEX t_c RESET (key_order)|uid=1 url=1'
	'rule:reindex: the same order, it shares again|REINDEX INDEX t_c|uid=2 url=2'
	'vacuum full of the table|VACUUM FULL t|uid=2 url=2'
	'cluster of the table by its primary key|CLUSTER t USING t_pkey|uid=2 url=2'
	'vacuum full of a store|VACUUM FULL t_url_sift_values|uid=2 url=2'
	'vacuum of the table|VACUUM t|uid=2 url=2'
	'a column type changes|ALTER TABLE t ALTER COLUMN url TYPE varchar(80)|uid=2 url=2'
	'drop a member: the last but one, the store stays|DROP INDEX t_c|uid=1 url=1'
	'>drop the last member: the stores go with it|DROP INDEX t_d|none'
	'>a build after it makes new stores|CREATE INDEX t_e ON t USING sift_col (id, uid, url)|uid=1 url=1'
	'a second member shares them|CREATE INDEX t_f ON t USING sift_col (uid, id, url)|uid=2 url=2'
	'drop one of two members|DROP INDEX t_e|uid=1 url=1'
)
for row in "${STEPS[@]}"; do
	IFS='|' read -r label stmt want <<<"$row"
	new=; [[ $label == '>'* ]] && { new=1; label=${label#>}; }
	pre=(); [[ $label == rule:* ]] && { pre=("SET sift.test_vstore_columns = ''"); label=${label#rule:}; }
	check "$label: the statement runs" sql primary "${pre[@]}" "$stmt"
	check "$label: equal answers" equal_answers
	check "$label: stores: $want" stores_are "$want"
	if [[ -n $new ]]; then BASE=$(fp); else check "$label: the values of the stores are the same" values_same; fi
done
check "the drops left no dependency to a relation that is gone" test "$(dangling primary)" == 0

# --- 2a. a build that waits for the drop of the last member makes a fresh store (the one of the drop is gone when it gets the lock)
blocked() { [[ $(sql primary "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query = '$1'") -ge 1 ]]; }
# race HOLD WAITER: session 1 runs HOLD in a transaction and holds it (it loads the library first: a session that has not loaded it
# keeps the store when it drops the last member, which the plain steps check); session 2 runs WAITER, which has to wait for it; then session 1 commits
race() {
	local r1 r2
	rm -f "$D/held" "$D/release"
	{ echo "LOAD 'pg_sift';"; echo "BEGIN;"; echo "$1;"; echo "\\! touch '$D/held'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.1; done"; echo "COMMIT;"; } | sql primary >"$D/s1.out" 2>&1 &
	S1=$!
	wait_for "[[ -e $D/held ]]" 120
	sql primary "$2" >"$D/s2.out" 2>&1 &
	S2=$!
	wait_for "blocked '$2'" 120 && WAITED=1 || WAITED=
	touch "$D/release"
	wait "$S1"; r1=$?
	wait "$S2"; r2=$?
	RACE_RC="$r1/$r2"
	out="session 1 ($r1): $(cat "$D/s1.out"); session 2 ($r2): $(cat "$D/s2.out")"
	[[ $r1 == 0 && $r2 == 0 ]] && ! grep -q deadlock "$D/s1.out" "$D/s2.out"
}
BASE=$(fp)
old=$(store_oids)
check "race: DROP INDEX of the last member, a build waits for the lock, the drop commits: both succeed, no deadlock" \
	race "DROP INDEX t_f" "CREATE INDEX t_g ON t USING sift_col (id, uid, url)"
check "race: the build waited for the lock of the drop" test -n "$WAITED"
new=$(store_oids)
check "race: the newcomer made fresh stores (the old ones went with the last member)" test "$new" != "$old" -a "${new%/*}" != "${old%/*}" -a "${new#*/}" != "${old#*/}"
check "race: the newcomer is the only member of the new stores" stores_are "uid=1 url=1"
check "race: equal answers" equal_answers
check "race: no dependency to a relation that is gone" test "$(dangling primary)" == 0
BASE=$(fp)

# --- 2b. the other order: a build adopts the store, the drop of the last old member waits for it: the store stays
old=$(store_oids)
check "race: a build adopts the store, the drop of the last old member waits for it, the build commits: both succeed" \
	race "CREATE INDEX t_h ON t USING sift_col (id, uid, url)" "DROP INDEX t_g"
check "race: the drop waited for the lock of the build" test -n "$WAITED"
check "race: the stores stayed (the newcomer uses them)" test "$(store_oids)" == "$old"
check "race: the newcomer is the only member" stores_are "uid=1 url=1"
check "race: the values of the stores are the same" values_same
check "race: equal answers" equal_answers
check "race: no dependency to a relation that is gone" test "$(dangling primary)" == 0

# --- 3. a reader while members are dropped, reindexed and promoted: the indexes answer as the heap in every transaction (repeatable read: the rows that are inserted
# meanwhile are in both or in none)
new_rows() { # COUNT TAG: rows with strings that the stores do not have
	sql primary "INSERT INTO t SELECT m.v + g, g % 7, md5('$2' || g)::uuid, 'https://example.com/n/' || md5('$2' || g)
		FROM (SELECT max(id) AS v FROM t) m, generate_series(1, $1) g"
}
reader() {
	local n=0 r
	while [[ ! -e $D/stop ]]; do
		r=$(sql primary "BEGIN ISOLATION LEVEL REPEATABLE READ" "SELECT vd_digest(true) = vd_digest(false)" "COMMIT" 2>&1)
		[[ $r == t ]] || echo "$r" >>"$D/reader.fail"
		echo $((++n)) >"$D/reader.n"
	done
}
sql primary "CREATE INDEX t_i ON t USING sift_col (id, uid, url)" "CREATE INDEX t_j ON t USING sift_col (id, tenant, uid, url)" \
	"CREATE INDEX t_k ON t USING sift_col (uid, url, tenant)" >/dev/null || bail "members for the reader failed"
check "reader: four members share the stores" stores_are "uid=4 url=4"
BASE=$(fp)
rm -f "$D/stop" "$D/reader.fail" "$D/reader.n"
reader &
READER=$!
wait_for "[[ -s $D/reader.n ]]" 120
check "reader: DROP INDEX CONCURRENTLY of a member" sql primary "DROP INDEX CONCURRENTLY t_i"
check "reader: REINDEX INDEX CONCURRENTLY of another" sql primary "REINDEX INDEX CONCURRENTLY t_j"
check "reader: rows with new strings" new_rows $((ROWS / 20)) n1
check "reader: sift.col_promote of a third member (appends codes to the stores)" sql primary "SELECT sift.col_promote('t_k')"
check "reader: sift.col_merge of it" sql primary "SELECT sift.col_merge('t_k', true)"
n=$(cat "$D/reader.n"); wait_for "[[ \$(cat $D/reader.n) -ge $((n + 2)) ]]" 120
touch "$D/stop"
wait "$READER"
check "reader: the indexes answered as the heap in all $(cat "$D/reader.n") rounds, no error" test ! -s "$D/reader.fail"
[[ ! -s $D/reader.fail ]] || diag "$(head -n 5 "$D/reader.fail")"
check "reader: the three members left share the stores" stores_are "uid=3 url=3"
check "reader: the codes of the start are still the same ones (the stores only grew)" values_kept
check "reader: equal answers" equal_answers
check "reader: no dependency to a relation that is gone" test "$(dangling primary)" == 0

# --- 4. kill -9. Members: t_h, t_j, t_k
PM=
restart() { # the primary is down (restart_after_crash is off): up again, the standby streaming
	wait_for '! kill -0 "$PM" 2>/dev/null' 120 && node_start primary && catchup primary standby
}
# after the kill: LABEL, the stores wanted
recovered() {
	if restart; then ok "$1: the primary restarts and the standby streams again"; else not_ok "$1: the primary restarts and the standby streams again"; return 1; fi
	check "$1: equal answers after recovery" equal_answers
	check "$1: stores: $2" stores_are "$2"
}
BASE=$(fp)
PM=$(head -1 "$D/data/postmaster.pid")
rm -f "$D/held"
{ echo "BEGIN;"; echo "DROP INDEX t_h;"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
HELD=$!
wait_for "[[ -e $D/held ]]" 120
crash primary
wait "$HELD"
recovered "an uncommitted DROP INDEX, killed" "uid=3 url=3"
check "an uncommitted DROP INDEX, killed: the member is still there" exists_on_both t_h t
check "an uncommitted DROP INDEX, killed: the values of the stores are the same" values_same

PM=$(head -1 "$D/data/postmaster.pid")
sql primary "DROP INDEX t_k" >/dev/null
crash primary
recovered "a committed DROP INDEX, killed at once" "uid=2 url=2"
check "a committed DROP INDEX, killed at once: the member is gone" exists_on_both t_k f
check "a committed DROP INDEX, killed at once: the values of the stores are the same" values_same
sql primary "CREATE INDEX t_k ON t USING sift_col (uid, url, tenant)" >/dev/null

# REINDEX of a member, the leader killed before and after its first directory update
for k in 1 -1; do
	what="REINDEX killed in the leader $([[ $k -gt 0 ]] && echo before || echo after) its update ${k#-}"
	PM=$(head -1 "$D/data/postmaster.pid")
	SIFT_TMP=$D leak_expect 1 'terminated by signal 9'
	if sql primary "SET sift.test_crash_dir = $k" "REINDEX INDEX t_j" >/dev/null 2>&1; then not_ok "$what: the process was not killed"; continue; fi
	recovered "$what" "uid=3 url=3" || continue
	check "$what: the values of the stores are the same" values_same
	check "$what: the same REINDEX run again succeeds" sql primary "REINDEX INDEX t_j"
	check "$what: equal answers after it" equal_answers
done

# rows with new strings, then sift.col_promote of a member, killed at the n-th directory update; a point that is not reached completes
for k in 1 -1 2 -2; do
	what="col_promote after new rows, killed in the leader $([[ $k -gt 0 ]] && echo before || echo after) update ${k#-}"
	new_rows $((ROWS / 40)) "k$k" >/dev/null || bail "rows failed"
	BASE=$(fp)
	PM=$(head -1 "$D/data/postmaster.pid")
	SIFT_TMP=$D leak_expect 1 'terminated by signal 9'
	if sql primary "SET sift.test_crash_dir = $k" "SELECT sift.col_promote('t_j')" >/dev/null 2>&1; then
		skip 1 "$what: it publishes fewer updates"
		continue
	fi
	recovered "$what" "uid=3 url=3" || continue
	check "$what: the codes of before are still the same ones" values_kept
	check "$what: the standby answers as the primary" equal_answers
done
check "the kills left no dependency to a relation that is gone" test "$(dangling primary)" == 0

# --- 5. pg_dump and pg_restore: the store relation of the dump (empty, access method sift_values) and the members that adopt it
MEMBERS=$(sql primary "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam
	WHERE a.amname = 'sift_col' AND i.indrelid = 't'::regclass")
check "dump: the source has three members on two stores" test "$MEMBERS" == 3 -a "$(stores primary)" == "uid=3 url=3"
PGARGS=(-h "$D/sock" -p "$(port primary)" -U postgres)
pg_dump "${PGARGS[@]}" -Fc -f "$D/src.custom" postgres 2>"$D/dump.err"
check "dump: pg_dump -Fc succeeds" test $? -eq 0 -a ! -s "$D/dump.err"
dumped() { pg_restore -f - "$D/src.custom" | grep -c "$1"; }
check "dump: the stores are created without data" test "$(dumped 'CREATE MATERIALIZED VIEW .*sift_values')|$(dumped 'WITH NO DATA')" == "2|2"
check "dump: no store is refreshed" test "$(dumped 'REFRESH MATERIALIZED VIEW')" == 0
restore() { # DB [pg_restore flags]: a new database on the primary, the archive restored into it
	local db=$1
	shift
	dbq primary postgres "CREATE DATABASE $db" >/dev/null || return 1
	out=$(PGOPTIONS="-c client_min_messages=warning" pg_restore "${PGARGS[@]}" "$@" -d "$db" "$D/src.custom" 2>&1)
	! grep -q 'ERROR\|pg_restore: error' <<<"$out"
}
restored() { # DB: the restored table as the source was
	local db=$1 names
	out=
	check "restore $db: pg_restore runs without errors" restore "$db" ${2:+"$2"}
	out=; check "restore $db: the table has its $MEMBERS sift_col indexes" test "$(dbq primary "$db" "SELECT count(*) FROM pg_index i
		JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_col' AND i.indrelid = 't'::regclass")" == "$MEMBERS"
	names=$(dbq primary "$db" "SELECT string_agg(c.relname || ':' || c.relkind::text, ',' ORDER BY c.relname) FROM pg_class c WHERE c.relname LIKE '%sift_values%'")
	out=; check "restore $db: exactly one store relation for each shared column (no copies of a name)" test "$names" == "t_uid_sift_values:m,t_url_sift_values:m"
	out=; check "restore $db: the stores serve all the members" stores_are "uid=$MEMBERS url=$MEMBERS" "$db"
	out=; check "restore $db: equal answers, on both nodes and every member verified" equal_answers "$db"
	out=; check "restore $db: no dependency to a relation that is gone" test "$(dbq primary "$db" "SELECT count(*) FROM pg_depend d
		WHERE d.refclassid = 'pg_class'::regclass AND d.refobjid >= 16384 AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = d.refobjid)")" == 0
}
restored r1
restored r2 -j4

# --- 2c. DROP TABLE and DROP SCHEMA .. CASCADE take the stores of their table and no other
cnt_stores() { sql "$1" "SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'sift_values' ${2:-}"; }
sql primary "CREATE SCHEMA sx" "CREATE TABLE sx.t (LIKE public.t)" "INSERT INTO sx.t SELECT * FROM public.t WHERE id <= $((ROWS / 4))" "ANALYZE sx.t" \
	"CREATE INDEX sx_a ON sx.t USING sift_col (id, uid, url)" "CREATE INDEX sx_b ON sx.t USING sift_col (uid, url, tenant)" >/dev/null || bail "schema failed"
BASE=$(fp)
check "schema: both tables have their two stores, under the same names" test "$(cnt_stores primary)" == 4 -a "$(cnt_stores primary "AND c.relnamespace = 'sx'::regnamespace")" == 2
check "schema: the other schema's stores are used by its own members" test "$(sql primary "SET search_path = sx, public" \
	"SELECT string_agg(column_name || '=' || indexes_using, ' ' ORDER BY column_name) FROM sift.col_value_store_status WHERE table_name = 'sx.t'::regclass")" == "uid=2 url=2"
check "schema: DROP SCHEMA .. CASCADE" sql primary "DROP SCHEMA sx CASCADE"
check "schema: its stores went, the other table's stayed" test "$(cnt_stores primary)" == 2 -a "$(catchup primary standby; cnt_stores standby)" == 2
check "schema: the table left keeps its stores and members" stores_are "uid=$MEMBERS url=$MEMBERS"
check "schema: the values of its stores are the same" values_same
check "schema: equal answers" equal_answers
check "table: DROP TABLE" sql primary "DROP TABLE t"
check "table: no relation of a store is left, on both nodes" test "$(catchup primary standby; sql primary "SELECT count(*) FROM pg_class WHERE relname LIKE '%sift_values%'")" == 0 \
	-a "$(sql standby "SELECT count(*) FROM pg_class WHERE relname LIKE '%sift_values%'")" == 0
check "table: no dependency to a relation that is gone" test "$(dangling primary)" == 0 -a "$(dangling standby)" == 0
check "table: the status view lists no store" test "$(sql primary "SELECT count(*) FROM sift.col_value_store_status")" == 0

echo "1..$NTEST"
finish
