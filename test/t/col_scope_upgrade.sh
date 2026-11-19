#!/usr/bin/env bash
# A scope index that the released 5.0 library wrote (scope segments: an integer key, a text key, a scope of two paths, and the cols(..) and max_multiplicity(..)
# options, with rows in the write buffers and pairs that changed) opens in this tree: until REINDEX its scope reads run the join (docs/UPGRADING.md, "5.0 to 5.1"),
# and writes, VACUUM, promotion, merges and the maintenance keep it readable with the answers of the join and of 5.0. REINDEX and REINDEX CONCURRENTLY leave the
# sidecar's column format and no scope segment, and a 5.0 library then refuses the index with the documented message ("Going back to 5.0"). The settings of 5.0
# that 5.1 removes are in the configuration of the node.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=$(git -C "$REPO" tag -l 'v5.0.*' --sort=-v:refname | head -n 1)
[[ -n $ref ]] || { echo '1..0 # SKIP no release tag v5.0.*'; exit 0; }
newroot=$ROOT
ext=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$REPO/pg_sift.control")
colfmt() { sed -n "s/^#define SIFT_COL_FORMAT_$1[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p" "$REPO/src/col/api/format.h"; }
ENTITY=$(colfmt SCOPE); TWOHOP=$(colfmt TWOHOP); CODED=$(colfmt SCOPEKEY); SEGMENTS=$(colfmt KEYGROUPS); SIDE=$(colfmt SIDE)
OLDER='scope segments of an older format (REINDEX)'
IXS="'events_col','tx_events_col','facts_col','ev_opts_col'"
plan 37

build_old "$ref"; oldroot=$OLDROOT
node_init us 64 "autovacuum = off
sift.async_maintenance = off
sift.col_unit_rows = 150
sift.scope_max_multiplicity = 64"
use_root us "$oldroot"
node_conf us "sift.col_auto_merge = off"
node_start us || bail "cannot start $ref"

sql us <<'SQL' >/dev/null || bail "the release cannot be set up"
CREATE EXTENSION pg_sift;
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 3) j ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, date '2026-01-01' + i % 300, i % 5, i % 100 FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, amount)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;

CREATE TABLE tx_scope (dim_a_id int NOT NULL, tenant_id text NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tx_scope SELECT i, 'tenant-' || (1 + i % 10) FROM generate_series(1, 300) i;
INSERT INTO tx_scope SELECT i, 'tenant-' || (1 + (i * 3 + 1) % 10) FROM generate_series(1, 300) i WHERE i % 7 = 0 ON CONFLICT DO NOTHING;
CREATE INDEX tx_scope_journal ON tx_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE tx_events (id int PRIMARY KEY, dim_a_id int, created_at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO tx_events SELECT id, dim_a_id, created_at, kind, amount FROM events;
CREATE INDEX tx_events_col ON tx_events USING sift_col (id, dim_a_id, created_at, kind, amount)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tx_scope(dim_a_id, tenant_id)');
REINDEX INDEX tx_events_col;

CREATE TABLE grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id);
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id);
CREATE TABLE memberships (group_id int, dim_a_id int, PRIMARY KEY (group_id, dim_a_id));
INSERT INTO grants SELECT m, 1 + m % 9, m, NULL FROM generate_series(1, 200) m;
INSERT INTO grants SELECT 200 + g, 1 + g % 9, NULL, g FROM generate_series(1, 20) g;
INSERT INTO memberships SELECT 1 + m % 20, m FROM generate_series(1, 210) m;
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + (i * 7919) % 220, date '2026-01-01' + i % 300, i % 5, i % 100 FROM generate_series(1, 6000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, kind, amount)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
REINDEX INDEX facts_col;

CREATE TABLE ev_opts AS SELECT * FROM events WHERE id <= 3000;
ALTER TABLE ev_opts ADD PRIMARY KEY (id);
CREATE INDEX ev_opts_col ON ev_opts USING sift_col (id, dim_a_id, created_at, kind, amount)
	WITH (key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id) cols(kind) max_multiplicity(64)');
REINDEX INDEX ev_opts_col;

-- after the build: rows in the write buffers, pairs that changed
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, date '2026-02-01' + i % 100, i % 5, i % 100 FROM generate_series(6001, 6400) i;
INSERT INTO tx_events SELECT id, dim_a_id, created_at, kind, amount FROM events WHERE id > 6000;
INSERT INTO facts SELECT i, 1 + (i * 7919) % 220, date '2026-02-01' + i % 100, i % 5, i % 100 FROM generate_series(6001, 6400) i;
INSERT INTO ev_opts SELECT * FROM events WHERE id BETWEEN 3001 AND 3200;
INSERT INTO tenant_scope SELECT m, 3 FROM generate_series(1, 300, 9) m ON CONFLICT DO NOTHING;
DELETE FROM tenant_scope WHERE dim_a_id % 11 = 0 AND tenant_id = 5;
UPDATE tenant_scope SET tenant_id = 13 WHERE dim_a_id % 13 = 0 AND tenant_id = 7;
INSERT INTO tx_scope SELECT m, 'tenant-4' FROM generate_series(2, 300, 11) m ON CONFLICT DO NOTHING;
DELETE FROM tx_scope WHERE dim_a_id % 17 = 0 AND tenant_id = 'tenant-5';
INSERT INTO grants SELECT 300 + m, 3, m, NULL FROM generate_series(201, 210) m;
DELETE FROM memberships WHERE group_id = 4 AND dim_a_id % 3 = 0;
INSERT INTO memberships SELECT 4, m FROM generate_series(211, 215) m;

CREATE FUNCTION scope_members(scope text, ks text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE scope WHEN 'grants'
		THEN 'SELECT g.dim_a_id FROM grants g WHERE g.tenant_id IN (' || ks || ') UNION SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.tenant_id IN (' || ks || ')'
		ELSE 'SELECT dim_a_id FROM ' || scope || ' WHERE tenant_id IN (' || ks || ')' END $$;
CREATE TABLE shapes (name text PRIMARY KEY, q text);
INSERT INTO shapes
SELECT t.t || ' ' || f.name, format(f.q, t.tbl, scope_members(t.scope, CASE f.ks WHEN 1 THEN t.k WHEN 2 THEN t.k || ', ' || t.k2 ELSE t.k0 END))
FROM (VALUES
	('int',    'events',    'tenant_scope', '3',            '7',            '99'),
	('text',   'tx_events', 'tx_scope',     '''tenant-4''', '''tenant-9''', '''none'''),
	('opts',   'ev_opts',   'tenant_scope', '3',            '7',            '99'),
	('twohop', 'facts',     'grants',       '3',            '4',            '99')) t (t, tbl, scope, k, k2, k0),
	(VALUES
	('by kind',  1, 'SELECT kind, count(*) AS n, sum(amount) AS total FROM %1$s e WHERE e.dim_a_id IN (%2$s) GROUP BY kind'),
	('total',    1, 'SELECT count(*) AS n, sum(amount) AS total, min(id) AS lo, max(id) AS hi FROM %1$s e WHERE e.dim_a_id IN (%2$s)'),
	('two keys', 2, 'SELECT kind, count(*) AS n, sum(amount) AS total FROM %1$s e WHERE e.dim_a_id IN (%2$s) GROUP BY kind'),
	('latest',   1, 'SELECT id, created_at FROM %1$s e WHERE e.dim_a_id IN (%2$s) ORDER BY created_at DESC, id DESC LIMIT 20'),
	('month',    1, 'SELECT date_trunc(''month'', created_at) AS m, count(*) AS n FROM %1$s e WHERE e.dim_a_id IN (%2$s) GROUP BY 1'),
	('no pair',  3, 'SELECT count(*) AS n FROM %1$s e WHERE e.dim_a_id IN (%2$s)')) f (name, ks, q);

CREATE FUNCTION ans(q text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE r text; BEGIN EXECUTE format('SELECT coalesce(md5(string_agg(t::text, ''|'' ORDER BY t::text)), ''none'') FROM (%s) t', q) INTO r; RETURN r; END $$;
CREATE FUNCTION ans_sift(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = on SET sift.agg_force = on SET max_parallel_workers_per_gather = 0 RETURN ans(q);
CREATE FUNCTION ans_heap(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = off SET max_parallel_workers_per_gather = 0 RETURN ans(q);
VACUUM (ANALYZE) events, tx_events, facts, ev_opts, tenant_scope, tx_scope, grants, memberships;
CREATE TABLE answers50 AS SELECT name, ans_sift(q) AS via_sift, ans_heap(q) AS via_heap FROM shapes;
SQL
T=("SET sift.test_scope_entity = on" "SET sift.agg_force = on")
q() { sql us "$@"; }
formats() { q "SELECT string_agg(i || ':' || f, ' ' ORDER BY i) FROM unnest(ARRAY[$IXS]) i, LATERAL (SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) f FROM sift.col_segments(i::regclass) WHERE kind = 'immutable') s"; }
scope_segments() { q "SELECT coalesce(string_agg(i || ':' || gen || ':' || bytes, ' ' ORDER BY i, gen), '') FROM unnest(ARRAY[$IXS]) i, LATERAL sift.col_segments(i::regclass) s WHERE s.format = $SEGMENTS"; }
disagree() { q "$@" "SELECT coalesce(string_agg(name, ', ' ORDER BY name), '') FROM shapes WHERE ans_sift(q) <> ans_heap(q)"; }
unsound() { { q "SELECT sift.col_verify(i::regclass, true) FROM unnest(ARRAY[$IXS]) i"; q "SELECT c.index::text || ' ' || c.problem FROM unnest(ARRAY[$IXS]) i, LATERAL sift.check(i::regclass) c"; } 2>&1 | tr -d '\n'; }
runs() { q "$@" "SELECT string_agg(s.name || '=' || f.why || '/' || f.runs, ', ' ORDER BY s.name) FROM shapes s, LATERAL fallback(s.q) f WHERE s.name LIKE '% by kind'"; }
sound() { out="$(disagree "${T[@]}")|$(disagree)|$(unsound)"; [[ $out == '||' ]]; }
intact() { sound && [[ $(scope_segments) == "$SCOPE50" ]]; }
taken() { q "${T[@]}" "SELECT coalesce(string_agg(s.name, ', ' ORDER BY s.name), '') FROM shapes s, LATERAL fallback(s.q) f WHERE s.name LIKE '% by kind' AND f.runs > 0 AND ${1:-true}"; }

check "5.0 wrote the entity segments (minors of the scope, of two paths and of a coded key) and the scope segments of the four indexes" test "$(formats)" == "ev_opts_col:$ENTITY,$SEGMENTS events_col:$ENTITY,$SEGMENTS facts_col:$TWOHOP,$SEGMENTS tx_events_col:$CODED,$SEGMENTS"
check "5.0: its scope reads gave the answers of the join" test "$(q "SELECT count(*) FROM answers50 WHERE via_sift <> via_heap")" == 0
check "the write buffers hold rows and the pairs have changed since the build" test "$(q "SELECT count(*) FROM unnest(ARRAY[$IXS]) i, LATERAL sift.col_segments(i::regclass) WHERE kind = 'buffer' AND rows > 0")" == 4
SCOPE50=$(scope_segments)
node_stop us

use_root us "$newroot"
node_start us; rc=$?
check "this tree starts on the data of $ref with sift.scope_max_multiplicity in postgresql.conf" test $rc -eq 0
((rc == 0)) || bail "no server"
sql us "ALTER EXTENSION pg_sift UPDATE" "SELECT sift.version()" 2>"$TAPDIR/err.txt" >/dev/null
check "ALTER EXTENSION UPDATE to $ext" test "$(q "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == "$ext"
check "a session says that the removed setting is dropped, as a WARNING" grep -q 'invalid configuration parameter name "sift.scope_max_multiplicity", removing it' "$TAPDIR/err.txt"
check "... and so does the server log" grep -q 'WARNING:  invalid configuration parameter name "sift.scope_max_multiplicity", removing it' "$(dir us)/postgres.log"
check "sift.upgrade_check warns of the stored value" test "$(q "SELECT status FROM sift.upgrade_check() WHERE kind = 'setting' AND object = 'sift.scope_max_multiplicity'")" == warn
sed_i '/^sift\.scope_max_multiplicity = /d' "$(dir us)/data/postgresql.conf"
node_stop us; node_start us
check "restarted without the line, a session starts with no warning" test -z "$(q "SELECT sift.version()" 2>&1 >/dev/null)"

check "the indexes open and verify (sift.col_verify, sift.check)" test -z "$(unsound)"
check "the scope reads give the join's rows, with the entity arm forced and as planned" test "$(disagree "${T[@]}")|$(disagree)" == "|"
check "... and 5.0's, recorded before the stop" test "$(q "${T[@]}" "SELECT count(*) FROM shapes s JOIN answers50 a USING (name) WHERE ans_sift(s.q) IS DISTINCT FROM a.via_sift OR ans_heap(s.q) IS DISTINCT FROM a.via_heap")" == 0
q "CREATE FUNCTION fallback(q text, OUT why text, OUT runs int) LANGUAGE plpgsql SET sift.enable_agg = on SET sift.agg_force = on SET max_parallel_workers_per_gather = 0 AS \$\$
DECLARE p jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	why := coalesce(jsonb_path_query_first(p, '\$.**.\"Scope Fallback\"') #>> '{}', 'none');
	runs := coalesce((jsonb_path_query_first(p, '\$.**.\"Scope Sidecar Runs\"') #>> '{}')::int, 0);
END \$\$" >/dev/null
check "EXPLAIN says that the scope segments are of an older format, and no run of a sidecar is read" \
	test "$(runs "${T[@]}")" == "int by kind=$OLDER/0, opts by kind=$OLDER/0, text by kind=$OLDER/0, twohop by kind=$OLDER/0"
check "as planned the join runs and says the same" test "$(runs)" == "int by kind=$OLDER/0, opts by kind=$OLDER/0, text by kind=$OLDER/0, twohop by kind=$OLDER/0"

more_writes() {
	q "INSERT INTO events SELECT i, 1 + (i * 7919) % 320, date '2026-03-01' + i % 50, i % 5, i % 100 FROM generate_series($1, $1 + 199) i" \
		"INSERT INTO tx_events SELECT id, dim_a_id, created_at, kind, amount FROM events WHERE id >= $1" \
		"INSERT INTO facts SELECT i, 1 + (i * 7919) % 220, date '2026-03-01' + i % 50, i % 5, i % 100 FROM generate_series($1, $1 + 199) i" \
		"INSERT INTO ev_opts SELECT * FROM events WHERE id BETWEEN $1 AND $1 + 199" \
		"INSERT INTO tenant_scope SELECT m, 4 FROM generate_series(2, 300, $2) m ON CONFLICT DO NOTHING" "DELETE FROM tenant_scope WHERE dim_a_id % $2 = 0 AND tenant_id = 3" \
		"INSERT INTO tx_scope SELECT m, 'tenant-9' FROM generate_series(3, 300, $2) m ON CONFLICT DO NOTHING" "UPDATE tx_scope SET tenant_id = 'tenant-$2' WHERE dim_a_id % 23 = 0 AND tenant_id = 'tenant-4'" \
		"INSERT INTO grants SELECT $1 + m, 5, m, NULL FROM generate_series($2 * 4, $2 * 4 + 20) m ON CONFLICT DO NOTHING" "DELETE FROM memberships WHERE group_id = 5 AND dim_a_id % 4 = $2 % 4"
}
more_writes 6401 19
check "writes after the open (rows in the buffers, pairs added, removed and re-keyed): the answers are the join's, the scope segments stay" intact
q "VACUUM events, tx_events, facts, ev_opts"
check "VACUUM: the answers are the join's, the scope segments stay" intact
check "... and it gives the entity segments of 16 units or more the sidecar (the minor of the sidecar), beside the scope segments of 5.0 that it leaves" \
	test "$(formats)" == "ev_opts_col:$SEGMENTS,$SIDE events_col:$SEGMENTS,$SIDE facts_col:$SEGMENTS,$SIDE tx_events_col:$SEGMENTS,$SIDE"
check "a read of an entity segment with a sidecar takes its runs, and still names the scope segments of the older format" \
	test "$(taken "f.why = '$OLDER'")" == "int by kind, opts by kind, text by kind, twohop by kind"
q "SELECT sift.col_promote(i::regclass) FROM unnest(ARRAY[$IXS]) i" >/dev/null
check "sift.col_promote: the answers are the join's, the scope segments stay" intact
q "SELECT sift.col_merge(i::regclass, true) FROM unnest(ARRAY[$IXS]) i" >/dev/null
check "sift.col_merge: the answers are the join's, the scope segments stay" intact
more_writes 6601 17
q "SELECT sift.col_maintain(i::regclass) FROM unnest(ARRAY[$IXS]) i" >/dev/null
check "sift.col_maintain: the answers are the join's, the scope segments stay" intact
check "the index of 5.0 is as big as 5.0's: no scope segment was dropped or rewritten, and none is of the sidecar's minor" test "$(scope_segments | wc -w)" == 4

PREFLIGHT=$newroot$(pg_config --sharedir)/extension/pg_sift_upgrade_check.sql
preflight() { psql -X -At -q -h "$(dir us)/sock" -p "$(port us)" -U postgres -d postgres -v to=5.0 -f "$PREFLIGHT" 2>&1 | grep -E "^(blocker|warn)\|[a-z_]+\|index\|"; }
q "SET client_min_messages = notice" "REINDEX INDEX ev_opts_col" 2>"$TAPDIR/err.txt" >/dev/null
check "REINDEX of the index that keeps cols(..) and max_multiplicity(..): the NOTICE says that each is ignored, and the options stay" \
	test "$(grep -c 'option is ignored' "$TAPDIR/err.txt")|$(q "SELECT reloptions::text ~ 'cols\(kind\) max_multiplicity\(64\)' FROM pg_class WHERE relname = 'ev_opts_col'")" == "2|t"
q "REINDEX INDEX events_col" "REINDEX INDEX tx_events_col"
check "REINDEX: the entity segments are of the sidecar's minor (a text key too) and no scope segment is left" \
	test "$(formats | tr ' ' '\n' | grep -v facts_col | tr '\n' ' ')|$(scope_segments | wc -w)" == "ev_opts_col:$SIDE events_col:$SIDE tx_events_col:$SIDE |1"
check "... the answers are the join's and the index verifies" sound
check "... the scope reads take the runs of the sidecar and name no fallback" test "$(taken "f.why = 'none'")" == "int by kind, opts by kind, text by kind"
check "sift.col_scope_verify finds nothing wrong" test -z "$(q "SELECT problem FROM sift.col_scope_verify('events_col', 100)" "SELECT problem FROM sift.col_scope_verify('tx_events_col', 100)")"
q "REINDEX INDEX CONCURRENTLY facts_col" "REINDEX INDEX CONCURRENTLY ev_opts_col"
rebuilt() { [[ -z $(scope_segments) ]] && sound; }
check "REINDEX CONCURRENTLY: no scope segment is left, the answers are the join's and the index verifies" rebuilt
q "VACUUM events, tx_events, facts, ev_opts"
check "the maintenance gives it the sidecar (the minor of the sidecar); a scope of two paths too" test "$(formats)" == "ev_opts_col:$SIDE events_col:$SIDE facts_col:$SIDE tx_events_col:$SIDE"
check "... and the answers are still the join's" sound

check "sift.upgrade_check('5.0') on the preflight file lists the indexes that 5.0 refuses, and no other" \
	test "$(preflight | cut -d'|' -f1,2 | tr '\n' ' ')" == "blocker|ev_opts_col blocker|events_col blocker|facts_col blocker|tx_events_col "
check "... and advises REINDEX with the 5.0 library, not a new index" test "$(preflight | grep -c 'REINDEX INDEX CONCURRENTLY')|$(preflight | grep -c 'DROP INDEX')" == "4|0"

node_stop us
use_root us "$oldroot"
node_conf us "sift.col_auto_merge = off
sift.scope_max_multiplicity = 64"
node_start us; rc=$?
check "$ref starts on the data again" test $rc -eq 0
((rc == 0)) || bail "no server"
out=$(for t in events tx_events ev_opts facts; do sql us "SET sift.enable_agg = off" "SELECT count(*) FROM $t" 2>&1; done)
check "5.0 refuses the four indexes that hold the sidecar's minor with the format message and the hint, and reads no row" \
	test "$(grep -c "has a segment of column format 1.$SIDE, this build reads 1.3 to 1.$SEGMENTS" <<<"$out")|$(grep -c 'HINT:  Use a newer pg_sift, or REINDEX.' <<<"$out")|$(grep -c '^[0-9]' <<<"$out")" == "4|4|0"
q "REINDEX INDEX CONCURRENTLY events_col" "REINDEX INDEX CONCURRENTLY tx_events_col" "REINDEX INDEX CONCURRENTLY ev_opts_col" "REINDEX INDEX CONCURRENTLY facts_col" 2>&1
check "REINDEX CONCURRENTLY with the 5.0 library builds the entity segments and the scope segments again" \
	test "$(formats)" == "ev_opts_col:$ENTITY,$SEGMENTS events_col:$ENTITY,$SEGMENTS facts_col:$TWOHOP,$SEGMENTS tx_events_col:$CODED,$SEGMENTS"
check "... and 5.0 answers as the join, with every index verified" test "$(q "SELECT count(*) FROM shapes WHERE ans_sift(q) <> ans_heap(q)")" == 0
check "5.0: sift.col_verify finds nothing" test -z "$(unsound)"
finish
