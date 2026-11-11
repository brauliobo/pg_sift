#!/usr/bin/env bash
# A sift_col index with the reloption scope (a second one with two scopes) restored from pg_dump: the custom archive serially and in parallel (eight runs), and a plain dump through psql. The text of the option is in
# the dump, the relation of the pairs and its unique btree (and the index that journals it) are made before the index that reads them (it depends on them), and the restore ends with the same index: the same
# options, the dependency on the btree (DROP INDEX of it is refused), the scope segments written by the restore (the scope entries are the join of the facts and the pairs), and the reads of one key, a set
# of keys counted once and per pair are the plain join's, with the reader's own verdict (the first scope index of a relation leaves its stamps unfilled until a REINDEX of it, which the restore does not do: those reads are the entity rows', and exact). A REINDEX of the restored index keeps all of it.
# Two indexes with a scope of two paths (lib_twohop.bash) go through the same restores: the btree that the rule chooses for each of the three relations is chosen again by the CREATE INDEX of the restore
# (a NOTICE for each), the one a `using` names is the one the dump says, and each restored index has the same edges to the three btrees and the three journals, the same entries and reads.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_twohop.bash"

plan 16
node_init scd 51 "$TWOHOP_CONF"
node_start scd || bail "start failed"
D=$(dir scd)
S=("-h" "$D/sock" "-p" "$(port scd)" "-U" "postgres")
export PGOPTIONS="-c client_min_messages=warning"
dbq() { local db=$1; shift; psql -X -At -q -v ON_ERROR_STOP=1 "${S[@]}" -d "$db" "${@/#/--command=}"; }
errors() { grep -c 'ERROR:\|pg_restore: error' || true; }
fresh() { dbq postgres "DROP DATABASE IF EXISTS $1" "CREATE DATABASE $1" >/dev/null; }

# the index as a user sees it: the options, the edges to the btrees, the entries against the join, and the reads against the plain join with and without the switch
OPTS="SELECT string_agg(o, ' ' ORDER BY o) FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'events_col' AND o LIKE 'scope=%'"
EDGES="SELECT coalesce(string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text), 'none') FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'events_col'::regclass
	AND refclassid = 'pg_class'::regclass AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i'"
ENTRIES="SELECT (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('events_col'))
	= (SELECT md5(string_agg(e.ctid::text || ':' || s.tenant_id, ',' ORDER BY e.ctid, s.tenant_id)) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id)"
READ="SET sift.agg_force = on"
Q_ONE="SELECT count(*), sum(v), min(id) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, 5)"
P_ONE="SELECT count(*), sum(v), min(id) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 5"
Q_SET="SELECT count(*), sum(v) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, ARRAY[2, 5, 9]::int8[])"
P_SET="SELECT count(*), sum(v) FROM events e WHERE EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id IN (2, 5, 9))"
Q_PAIR="SELECT k, (SELECT count(*) FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, k)) FROM generate_series(1, 13) k ORDER BY k"
P_PAIR="SELECT k, (SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k) FROM generate_series(1, 13) k ORDER BY k"
reads() { # DB: the scope forms and the join, one string each, joined by the verdict of their comparison
	local db=$1
	[[ $(dbq "$db" "$READ" "$Q_ONE" "$Q_SET" "$Q_PAIR") == "$(dbq "$db" "SET sift.enable_agg = off" "$P_ONE" "$P_SET" "$P_PAIR")" ]] && echo same || echo DIFFERENT
}
state() {
	local db=$1
	echo "$(dbq "$db" "$OPTS") | $(dbq "$db" "$EDGES") | entries $(dbq "$db" "$ENTRIES") | reads $(reads "$db")" \
		"| refused $(dbq "$db" "DO \$\$ BEGIN DROP INDEX tenant_scope_pkey; EXCEPTION WHEN dependent_objects_still_exist THEN RAISE WARNING 'refused'; END \$\$" 2>&1 | grep -c refused)"
}

TWOHOP_SQL=dbq
fresh src
out=$(dbq src "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 12 FROM generate_series(1, 400) m, generate_series(0, 5) j WHERE j < m % 7 ON CONFLICT DO NOTHING" \
	"CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE group_scope (dim_a_id int NOT NULL, group_id int NOT NULL, UNIQUE (group_id, dim_a_id)) WITH (autovacuum_enabled = false)" \
	"INSERT INTO group_scope SELECT m, 1 + m % 30 FROM generate_series(1, 400) m" \
	"CREATE INDEX group_scope_journal ON group_scope USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 420 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 60) FROM generate_series(1, 9000) i" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')" \
	"CREATE TABLE events2 (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events2 SELECT id, dim_a_id, created_at, v FROM events" \
	"CREATE INDEX events_two ON events2 USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id); group_id: dim_a_id -> group_scope(dim_a_id, group_id)')" \
	"VACUUM (FREEZE, ANALYZE) events, events2, tenant_scope, group_scope" 2>&1) || bail "setup failed: $(tail -3 <<<"$out")"
dbq src "${TWOHOP_STMTS[@]}" "CREATE UNIQUE INDEX grants_direct_all ON grants (dim_a_id, tenant_id)" \
	"CREATE INDEX hop_using ON hop_events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) using grants_direct_all | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)')" \
	"REINDEX INDEX hop_using" "VACUUM (FREEZE, ANALYZE) hop_events, grants, memberships" >/dev/null || bail "the scopes of two paths failed"
hopstate() { # DB: of both indexes, the option, the edges to the btrees and the journals, the entries, the reads, and how many of the two btrees of the direct grants cannot be dropped
	local db=$1 ix out=
	for ix in hop_col hop_using; do
		out+="$ix: $(dbq "$db" "${OPTS//events_col/$ix}") | $(dbq "$db" "${EDGES//events_col/$ix}") | entries $(dbq "$db" "${TWOHOP_ENTRIES//hop_col/$ix}") | reads $(TWOHOP_IX=$ix twohop_reads "$db"); "
	done
	echo "${out}refused $(dbq "$db" "DO \$\$ DECLARE b text; BEGIN FOREACH b IN ARRAY ARRAY['grants_direct', 'grants_direct_all'] LOOP BEGIN EXECUTE 'DROP INDEX ' || b; EXCEPTION WHEN dependent_objects_still_exist THEN RAISE WARNING 'refused'; END; END LOOP; END \$\$" 2>&1 | grep -c refused)"
}
SRC=$(state src)
WANT="scope=tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) | tenant_scope_journal tenant_scope_pkey | entries t | reads same | refused 1"
check "the source: the option, the dependencies on the journal index and the btree of the pairs, the entries are the join, the reads are the join's" test "$SRC" == "$WANT"
TWO="SELECT count(*) FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'events_two'::regclass AND refclassid = 'pg_class'::regclass AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i'"
check "the index with two scopes depends on both btrees and both journal indexes" test "$(dbq src "$TWO")" == 4
HOP=$(hopstate src)
check "the source: both scopes of two paths depend on the three btrees (the rule's, and the one that using names) and the three journals, the entries and the reads are the join's" test "$HOP" == \
	"hop_col: scope=$TWOHOP_SCOPE | grants_by_group grants_by_member grants_direct grants_group memberships_active memberships_by_member | entries t | reads same; hop_using: scope=tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) using grants_direct_all | grants(group_id, tenant_id) via memberships(group_id, dim_a_id) | grants_by_group grants_by_member grants_direct_all grants_group memberships_active memberships_by_member | entries t | reads same; refused 2"
pg_dump "${S[@]}" -Fc -f "$D/src.custom" src
pg_dump "${S[@]}" -Fp -f "$D/src.sql" src
check "pg_dump writes the text of the option" test "$(grep -c "scope='tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'" "$D/src.sql")" == 1
line() { grep -n "$1" "$D/src.sql" | head -n 1 | cut -d: -f1; }
check "pg_dump writes the text of both scopes of two paths, with the using of one" test "$(grep -c "scope='$TWOHOP_SCOPE'" "$D/src.sql")" == 1 -a "$(grep -c "using grants_direct_all | grants(group_id" "$D/src.sql")" == 1
check "the btrees of the pairs are made before the indexes that read them" \
	test "$(line 'tenant_scope_pkey PRIMARY KEY')" -lt "$(line 'CREATE INDEX events_col ')" -a "$(line 'group_scope_group_id_dim_a_id_key')" -lt "$(line 'CREATE INDEX events_two ')"
check "the index that journals the pairs is made before the index that reads them" test "$(line 'CREATE INDEX tenant_scope_journal')" -lt "$(line 'CREATE INDEX events_col ')"

fresh r1
check "pg_restore, serial: no error and the same index" test "$(pg_restore "${S[@]}" -d r1 "$D/src.custom" 2>&1 | errors)" == 0 -a "$(state r1)" == "$WANT"
check "pg_restore, serial: the scopes of two paths have the edges, entries and reads of the source" test "$(hopstate r1)" == "$HOP"

ok_runs=0
for run in 1 2 3 4 5 6 7 8; do
	fresh rp
	out=$(pg_restore "${S[@]}" -j8 -d rp "$D/src.custom" 2>&1)
	[[ $(errors <<<"$out") == 0 && $(state rp) == "$WANT" && $(dbq rp "$TWO") == 4 && $(hopstate rp) == "$HOP" ]] && ok_runs=$((ok_runs + 1)) || diag "run $run: $(grep -m1 -A3 'ERROR' <<<"$out" | tr '\n' ' ') state: $(state rp)"
done
check "pg_restore -j8, eight runs: no error and the same indexes" test "$ok_runs" == 8

fresh r2
check "pg_dump | psql: no error and the same index" test "$(psql -X -q "${S[@]}" -d r2 -f "$D/src.sql" 2>&1 | errors)" == 0 -a "$(state r2)" == "$WANT"

# the CREATE INDEX of the restore chooses the unique btree of each relation again and says it, the three of each index; the one that using names is the one the dump says
fresh r3
NOTICES=$(sed 's/^SET client_min_messages = warning;/SET client_min_messages = notice;/' "$D/src.sql" | psql -X -q "${S[@]}" -d r3 2>&1 | grep -E 'NOTICE:  scope "tenant_id": the (direct pairs|joined pairs|via relation) of')
check "pg_dump | psql: the restore says the btree of each of the three relations of both indexes, 'that satisfy' the predicate" test "$(grep -c . <<<"$NOTICES")" == 6 -a "$(grep -c 'that satisfy' <<<"$NOTICES")" == 5
check "... one of them is the btree that using names, and the rule's choice of the others is the source's" test "$(grep -c '"grants_direct_all"' <<<"$NOTICES")" == 1 -a "$(grep -c '"grants_direct" ' <<<"$NOTICES")" == 1 \
	-a "$(hopstate r3)" == "$HOP"
diag "$NOTICES"

# two scope indexes of one table made by a parallel restore
{
	dbq src "CREATE INDEX events_three ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')" >/dev/null
	pg_dump "${S[@]}" -Fc -f "$D/same.custom" src
	runs=0
	for run in 1 2 3 4 5 6; do fresh rs; [[ $(pg_restore "${S[@]}" -j8 -d rs "$D/same.custom" 2>&1 | errors) == 0 ]] && runs=$((runs + 1)); done
	check "pg_restore -j8 of two scope indexes of one table, six runs: no deadlock" test "$runs" == 6
}
dbq r2 "REINDEX INDEX events_col" "REINDEX INDEX events_two" "REINDEX INDEX hop_col" "REINDEX INDEX hop_using" >/dev/null 2>&1
check "REINDEX of the restored indexes keeps the options, the dependencies and the entries" test "$(state r2)" == "$WANT" -a "$(dbq r2 "$TWO")" == 4 -a "$(hopstate r2)" == "$HOP"
check "the restored indexes verify, and the first REINDEX of a scope of two paths gives it three stamps that vouch" test -z "$(dbq r2 "SELECT sift.col_verify('events_col', true)" "SELECT sift.col_verify('events_two', true)" \
	"SELECT sift.col_verify('hop_col', true)" "SELECT sift.col_verify('hop_using', true)" 2>&1)" -a "$(TWOHOP_SQL=dbq twohop_vouch r2)" == ok
finish
