#!/usr/bin/env bash
# pg_upgrade of a cluster with sift, sift_col and sift_table objects, some of them in a tablespace: pg_upgrade --check, the upgrade, the answers,
# sift.verify, sift.col_verify, the preflight of scripts/upgrade-check.sh, and writes, VACUUM and merges afterwards. From the previous major to this
# one when its binaries are installed (SIFT_PG_OLD_BIN, or the PGDG directory of the major before this one: PostgreSQL 17 to 18), with pg_sift built
# for that major from a copy of this tree; otherwise to the same major in another data directory.
#
# pg_upgrade loads '$libdir/pg_sift' in both clusters, which the staged install (scripts/pgtest.sh) is not in: each side runs from a copy of its
# binaries at the same absolute path under the test directory (a server finds its library and share directories relative to its binaries), with the
# files of the system linked in and pg_sift of the staged install, or of the build for the other major, next to them. The clusters have data
# checksums, which PostgreSQL 18 enables by default and pg_upgrade needs on both sides.
. "$(dirname "$0")/lib.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
. "$(dirname "$0")/lib_upgrade.bash"

NEWCFG=$(command -v pg_config)
OLDBIN=${SIFT_PG_OLD_BIN:-}
if [[ -z $OLDBIN ]]; then
	for c in "/usr/lib/postgresql/$((PG_MAJOR - 1))/bin" "/usr/pgsql-$((PG_MAJOR - 1))/bin"; do
		[[ -x $c/pg_config ]] && { OLDBIN=$c; break; }
	done
fi
# SIFT_PG_OLD_REF: the old cluster runs a released library (a tag or a release branch, built for the old major from its archive) and the objects it wrote with
# the options it has; after pg_upgrade the extension is updated to this tree's version as a user does. Unset: this tree on both sides.
OLD_REF=${SIFT_PG_OLD_REF:-}
if [[ -n $OLD_REF ]]; then
	git -C "$REPO" rev-parse --verify -q "$OLD_REF^{commit}" >/dev/null || { echo "1..0 # SKIP $OLD_REF is not in this repository"; exit 0; }
	[[ $(uname -m) == x86_64 || $(printf '%s\n' "${OLD_REF#v}" 3.2.0 | sort -V | head -1) == 3.2.0 ]] || { echo "1..0 # SKIP $OLD_REF builds on x86-64 only"; exit 0; }
fi
OLDCFG=${OLDBIN:+$OLDBIN/pg_config}
OLDCFG=${OLDCFG:-$NEWCFG}
OLD_MAJOR=$("$OLDCFG" --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
# a server image can carry a major pg_sift does not support (the Makefile's PG_MIN_MAJOR): the upgrade then runs within the major
if ((OLD_MAJOR < $(sed -n 's/^PG_MIN_MAJOR *= *\([0-9]*\).*/\1/p' "$REPO/Makefile" | head -n 1))); then OLDCFG=$NEWCFG; OLD_MAJOR=$PG_MAJOR; fi

TS=$TAPDIR/tablespace
OV=$TAPDIR/overlay
trap 'pg_ctl -D "$(dir old)/data" -m immediate stop >/dev/null 2>&1; cleanup; ((NFAIL)) || rm -rf "$(dir old)" "$OV" "$TS" "$TAPDIR/old-src"' EXIT
rm -rf "$TS" "$OV"
mkdir -p "$TS" "$OV"
diag "pg_upgrade from PostgreSQL $OLD_MAJOR to $PG_MAJOR"

# the servers: this major from the staged install, the old one from the same or a build of this tree for its major
overlay "$NEWCFG" "$OV/new" "$ROOT"
if ((OLD_MAJOR == PG_MAJOR)) && [[ -z $OLD_REF ]]; then
	overlay "$OLDCFG" "$OV/old" "$ROOT"
else
	build_old "$OLDCFG" "$OV/oldroot" || { (($? == 77)) && exit 0; tail -n 20 "$TAPDIR/old-build.log"; bail "pg_sift does not build for PostgreSQL $OLD_MAJOR"; }
	overlay "$OLDCFG" "$OV/old" "$OV/oldroot"
fi
plan $((30 + (${#OLD_REF} ? 3 : 0)))
OLDB=$OV/old$("$OLDCFG" --bindir)
NEWB=$OV/new$("$NEWCFG" --bindir)
PATH=$NEWB:$PATH

initdb_node old 30 "$OLDB"
initdb_node new 31 "$NEWB"
NODES+=(new)
node_conf new "$("$REPO/scripts/pgtest.sh" conf)"
LD_PRELOAD=${SIFT_PRELOAD:-} "$OLDB/pg_ctl" -D "$(dir old)/data" -l "$(dir old)/postgres.log" -w -t 120 start >/dev/null 2>&1 || bail "the old cluster does not start"

# a tablespace needs two catalog versions (pg_upgrade refuses the same one)
TSDDL=; TSC=
if ((OLD_MAJOR != PG_MAJOR)); then TSDDL="CREATE TABLESPACE ts1 LOCATION '$TS';"; TSC="TABLESPACE ts1"; fi

# the options of this tree's column index; a released library has none of them (cube_minmax and key_columns are 3.3's, hll_columns 3.4's)
FACTS_OPTS="WITH (cube_minmax = true, key_columns = 'g', hll_columns = 'id')"
[[ -z $OLD_REF ]] || FACTS_OPTS=
# 4.3 still honors the three options that 4.4 removed and keeps accepting: an index that stores them must come through pg_upgrade
[[ ! ${OLD_REF#v} =~ ^4\.3\. ]] || FACTS_OPTS="WITH (key_cut_rows = 500, point_columns = 'id', order_columns = 'ts')"
# the wide leaves (an enum, an interval, a uuid) are 3.2's: the index of the older releases covers the numbers
FACTS_COLS='id, g, n, t, w, u, ts'
[[ ! ${OLD_REF#v} =~ ^3\.[01]\. ]] || FACTS_COLS='id, g, n'
# an index of a release before 4.0 has no track_changes option and no triggers; this tree's builds the default (a primary key, two statement triggers)
TRKOPT=
# a connected pair of column indexes (the option connect is 4.3's): the child index depends on the parent index, which keeps a journal of the keys that changed
CONNSQL=
[[ -n $OLD_REF ]] || CONNSQL="CREATE TABLE cdim (id int PRIMARY KEY, cat int NOT NULL, label text) WITH (autovacuum_enabled = false);
INSERT INTO cdim SELECT g, g % 10, 'l' || g FROM generate_series(1, 500) g;
CREATE TABLE cdim2 (id int PRIMARY KEY, zone text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO cdim2 SELECT g, 'r' || g % 5 FROM generate_series(1, 50) g;
CREATE TABLE cfact (id int PRIMARY KEY, dim_id int NOT NULL, dim2_id int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO cfact SELECT g, 1 + g % 500, 1 + g % 50, g FROM generate_series(1, 2000) g;
CREATE INDEX cdim_c ON cdim USING sift_col (id, cat, label);
CREATE INDEX cdim2_c ON cdim2 USING sift_col (id, zone);
CREATE INDEX cfact_c ON cfact USING sift_col (id, dim_id, dim2_id, v)
  WITH (connect = 'dim_id -> cdim(id); dim2_id -> cdim2(id)');
UPDATE cdim SET cat = cat + 1 WHERE id % 50 = 0;"
# named projections (5.0's): two definitions, one index that serves both; the indexes are created by pg_upgrade before the data of sift.projections is there
PROJSQL=
[[ -n $OLD_REF ]] || PROJSQL="CREATE TABLE pev (id int PRIMARY KEY, a int, k int, q int, s text) WITH (autovacuum_enabled = false);
INSERT INTO pev SELECT g, g % 40, g % 5, g % 97, (ARRAY['new', 'open', 'held'])[1 + g % 3] FROM generate_series(1, 3000) g;
SELECT sift.projection_create('pwide', 'pev', ARRAY['a', 'k', '(q / 10)', 's'], rollups => 'a, k');
SELECT sift.projection_create('pnarrow', 'pev', ARRAY['a', 'k']);
SELECT sift.projection_index('pev_wide', ARRAY['pwide'], '{\"track_changes\": \"off\"}');
SELECT sift.projection_index('pev_again', ARRAY['pnarrow'], '{\"track_changes\": \"off\"}');"
# the objects: the three access methods, the types of the wide leaves and JSON paths, open write buffers and promoted segments, a tablespace
SETUP=$(cat <<SQL
CREATE EXTENSION pg_sift;
CREATE TYPE tone AS ENUM ('lo', 'mid', 'hi');
$TSDDL
CREATE TABLE docs (id int PRIMARY KEY, body text);
INSERT INTO docs SELECT g, concat_ws(' ', 'alpha' || g % 7, 'beta' || g % 13, CASE WHEN g % 3 = 0 THEN 'gamma' END) FROM generate_series(1, 4000) g;
CREATE INDEX docs_idx ON docs USING sift (body) WITH (write_buffer_limit = 64);
CREATE INDEX docs_ts_idx ON docs USING sift (body) $TSC;
INSERT INTO docs SELECT g, concat_ws(' ', 'alpha' || g % 7, 'late') FROM generate_series(4001, 4600) g;
UPDATE docs SET body = 'beta5 moved gamma' WHERE id % 97 = 0;
DELETE FROM docs WHERE id % 89 = 0;
CREATE TABLE facts (id int, g int, n numeric(12,2), t tone, w interval, u uuid, ts timestamptz, doc jsonb);
INSERT INTO facts SELECT g, g % 9, g % 1000 / 8.0, (ARRAY['lo', 'mid', 'hi'])[g % 3 + 1]::tone, make_interval(secs => g), md5(g::text)::uuid,
	timestamptz '2026-01-01' + g * interval '1 minute',
	jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || g % 4, 'qty', g % 5))) FROM generate_series(1, 5000) g;
CREATE INDEX facts_col ON facts USING sift_col ($FACTS_COLS) $FACTS_OPTS;
CREATE INDEX facts_json ON facts USING sift_col (id, doc) WITH (paths = 'doc.items[]{category text, qty int4}') $TSC;
CREATE TABLE tbl (id int, g int, n numeric(12,2), t tone) USING sift_table;
INSERT INTO tbl SELECT g, g % 9, g % 1000 / 8.0, (ARRAY['lo', 'mid', 'hi'])[g % 3 + 1]::tone FROM generate_series(1, 5000) g;
UPDATE tbl SET n = n + 1 WHERE id % 20 = 0;
DELETE FROM tbl WHERE id % 25 = 0;
CREATE TABLE tbl_ts (id int, g int, n int) USING sift_table $TSC;
INSERT INTO tbl_ts SELECT g, g % 9, g % 100 FROM generate_series(1, 5000) g;
VACUUM tbl_ts;
CREATE TABLE trk (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO trk SELECT g, g % 7, g FROM generate_series(1, 3000) g;
CREATE INDEX trk_c ON trk USING sift_col (id, g, v) $TRKOPT;
UPDATE trk SET id = id + 10000 WHERE id % 10 = 0;
DELETE FROM trk WHERE id % 13 = 0;
-- pg_dump restores an index after its table's key when its name sorts after the key's (before it on PostgreSQL 18, which sorts the statistics in: the other order): the build then
-- meets a key while the extension is still at the old version
CREATE TABLE trk_late (id int PRIMARY KEY, g int NOT NULL);
INSERT INTO trk_late SELECT g, g % 3 FROM generate_series(1, 300) g;
CREATE INDEX trk_late_z ON trk_late USING sift_col (id, g);
$CONNSQL
$PROJSQL
CREATE FUNCTION trkq() RETURNS text LANGUAGE sql RETURN (SELECT string_agg(g || ':' || c || ':' || s, ',' ORDER BY g) FROM (SELECT g, count(*) c, sum(v) s FROM trk GROUP BY g) x);
CREATE FUNCTION trkq_on() RETURNS text LANGUAGE sql SET sift.enable_agg = on SET cpu_tuple_cost = 0.5 RETURN trkq();
CREATE FUNCTION trkq_off() RETURNS text LANGUAGE sql SET sift.enable_agg = off RETURN trkq();
CREATE FUNCTION bm25q() RETURNS text LANGUAGE plpgsql SET enable_seqscan = off AS \$\$
DECLARE q text; r text; o text := ''; s record;
BEGIN
	FOREACH q IN ARRAY ARRAY['alpha3', 'beta5 gamma', 'alpha3 NOT beta5', 'w1*', 'gamma OR late', 'moved'] LOOP
		EXECUTE 'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM docs WHERE body @@> \$1' INTO r USING q;
		o := o || q || '=' || r || ';';
	END LOOP;
	FOR s IN SELECT id, round(sift.score(ctid)::numeric, 4) sc FROM docs WHERE body @@> 'alpha3 OR gamma' ORDER BY sift.score(ctid) DESC, id LIMIT 20 LOOP
		o := o || s.id || '@' || s.sc || ',';
	END LOOP;
	RETURN o;
END \$\$;
CREATE FUNCTION colq() RETURNS text LANGUAGE plpgsql AS \$\$
DECLARE o text; r text; q text;
BEGIN
	o := '';
	FOREACH q IN ARRAY ARRAY[
		'SELECT string_agg(g || '':'' || c || '':'' || s || '':'' || mn || '':'' || mx, '','' ORDER BY g) FROM (SELECT g, count(*) c, sum(n) s, min(id) mn, max(id) mx FROM facts GROUP BY g) x',
		'SELECT string_agg(t || '':'' || c || '':'' || mx, '','' ORDER BY t) FROM (SELECT t, count(*) c, max(w) mx FROM facts GROUP BY t) y',
		'SELECT string_agg(k || '':'' || c, '','' ORDER BY k) FROM (SELECT e.category k, count(*) c FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc->''items'') e(category text, qty int) GROUP BY 1) z'] LOOP
		EXECUTE q INTO r;
		o := o || r || '/';
	END LOOP;
	RETURN o;
END \$\$;
CREATE FUNCTION colq_on() RETURNS text LANGUAGE sql SET sift.enable_agg = on SET cpu_tuple_cost = 0.5 RETURN colq();
CREATE FUNCTION colq_off() RETURNS text LANGUAGE sql SET sift.enable_agg = off RETURN colq();
CREATE FUNCTION tblq(t regclass) RETURNS text LANGUAGE plpgsql AS \$\$
DECLARE o text;
BEGIN
	EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(n), 0) || '':'' || max(id) || '':'' || count(DISTINCT g) FROM %s', t) INTO o;
	RETURN o;
END \$\$;
CREATE FUNCTION tblq_on(t regclass) RETURNS text LANGUAGE sql SET sift.enable_agg = on SET cpu_tuple_cost = 0.5 RETURN tblq(t);
CREATE FUNCTION tblq_off(t regclass) RETURNS text LANGUAGE sql SET sift.enable_agg = off RETURN tblq(t);
SQL
)
qs old postgres <<<"$SETUP" >/dev/null || bail "the old cluster cannot be set up"
# stored settings of the database and of a role in it (settings both releases have): pg_upgrade carries them in the globals
q old postgres "ALTER DATABASE postgres SET sift.simd = 'scalar'" "ALTER ROLE postgres IN DATABASE postgres SET sift.enable_agg = on" >/dev/null || bail "stored settings"
VERIFY_SQL="SELECT sift.verify('docs_idx', true), sift.verify('docs_ts_idx', true), sift.col_verify('facts_col', true), sift.col_verify('facts_json', true), sift.col_verify('trk_c', true), sift.col_verify('trk_late_z', true)"
fingerprint() { q "$1" postgres "SELECT bm25q()" "SELECT colq_on() = colq_off()" "SELECT colq_off()" "SELECT tblq_off('tbl')" "SELECT tblq_on('tbl') = tblq_off('tbl')" \
	"SELECT tblq_off('tbl_ts')" "SELECT tblq_on('tbl_ts') = tblq_off('tbl_ts')"; }
VERSIONS=$(q old postgres "SELECT extversion || ' ' || sift.version() FROM pg_extension WHERE extname = 'pg_sift'")
BEFORE=$(fingerprint old)
TRK_BEFORE=$(q old postgres "SELECT trkq_off()")
check "the old cluster agrees with itself: the scans equal the normal plans" test "$(grep -c '^t$' <<<"$BEFORE")" == 3
CONNECTION="SELECT child || ' ' || parent || ' ' || declaration || ' ' || (lineage IS NOT NULL) || ' ' || records || ' ' || stamp FROM sift.col_connections ORDER BY ordinal"
[[ -n $OLD_REF ]] || { CONN_BEFORE=$(q old postgres "$CONNECTION"); RECS_BEFORE=$(q old postgres "SELECT sum(records) FROM sift.col_connections"); }
FILES_BEFORE=$(q old postgres "SELECT string_agg(relname || '=' || pg_relation_filepath(oid), ',' ORDER BY relname) FROM pg_class WHERE relname IN ('docs_ts_idx', 'facts_json', 'tbl_ts')")
if [[ -n $TSC ]]; then
	check "the tablespace holds an index of each access method and a table" test "$(grep -o 'pg_tblspc' <<<"$FILES_BEFORE" | wc -l | tr -d ' ')" == 3
else skip 1 "pg_upgrade to the same major cannot carry a tablespace"; fi
"$OLDB/pg_ctl" -D "$(dir old)/data" -m fast -w stop >/dev/null 2>&1 || bail "the old cluster does not stop"

# pg_upgrade --check changes nothing, the upgrade follows
UPG=("$NEWB/pg_upgrade" -b "$OLDB" -B "$NEWB" -d "$(dir old)/data" -D "$(dir new)/data" -U postgres -p "$(port old)" -P "$(port new)" -s "$(dir new)/sock")
(cd "$(dir new)" && LD_PRELOAD=${SIFT_PRELOAD:-} "${UPG[@]}" --check >"$(dir new)/pg_upgrade_check.log" 2>&1)
checked=$?
check "pg_upgrade --check passes" test $checked -eq 0
[[ $checked -eq 0 ]] || diag "$(tail -n 20 "$(dir new)/pg_upgrade_check.log"; cat "$(dir new)"/data/pg_upgrade_output.d/*/loadable_libraries.txt 2>/dev/null)"
(cd "$(dir new)" && LD_PRELOAD=${SIFT_PRELOAD:-} "${UPG[@]}" >"$(dir new)/pg_upgrade.log" 2>&1)
upgraded=$?
check "pg_upgrade succeeds" test $upgraded -eq 0
if [[ $upgraded -ne 0 ]]; then
	diag "$(tail -n 20 "$(dir new)/pg_upgrade.log"; tail -n 8 "$(dir new)"/data/pg_upgrade_output.d/*/log/pg_upgrade_server.log 2>/dev/null; tail -n 12 "$(dir new)"/data/pg_upgrade_output.d/*/log/pg_upgrade_dump_*.log 2>/dev/null)"
	bail "pg_upgrade failed"
fi
node_start new || bail "the upgraded cluster does not start"

if [[ -z $OLD_REF ]]; then
	check "the extension and the library are the ones of before" test "$(q new postgres "SELECT extversion || ' ' || sift.version() FROM pg_extension WHERE extname = 'pg_sift'")" == "$VERSIONS"
else
	# the catalog still says the release (pg_upgrade's: the update is the user's next step
	check "the extension is the release's" test "$(q new postgres "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == "$(cut -d' ' -f1 <<<"$VERSIONS")"
	check "the library is this tree's" test "$(q new postgres "SELECT sift.version()")" != "$(cut -d' ' -f2 <<<"$VERSIONS")"
	check "the answers are the same before the extension is updated" test "$(fingerprint new)" == "$BEFORE"
	check "ALTER EXTENSION UPDATE brings the extension to the version of this tree" test "$(q new postgres "ALTER EXTENSION pg_sift UPDATE" "SELECT extversion = default_version FROM pg_extension e, pg_available_extensions a WHERE e.extname = 'pg_sift' AND a.name = 'pg_sift'" | tail -n 1)" == t
fi
if [[ -n $TSC ]]; then
	check "the relations of the tablespace are under it, with the same file names" test "$(q new postgres "SELECT string_agg(relname || '=' || pg_relation_filepath(oid), ',' ORDER BY relname) FROM pg_class WHERE relname IN ('docs_ts_idx', 'facts_json', 'tbl_ts')" |
	sed 's#PG_[0-9]*_[0-9]*#V#g')" == "$(sed 's#PG_[0-9]*_[0-9]*#V#g' <<<"$FILES_BEFORE")"
else skip 1 "no tablespace"; fi
check "the stored settings of the database and of the role in it survived pg_upgrade" test "$(q new postgres "LOAD 'pg_sift'" "SHOW sift.simd" "SHOW sift.enable_agg" | tr '\n' ' ')" == "scalar on "
check "sift.verify and sift.col_verify pass on every index" quiet q new postgres "$VERIFY_SQL"
AFTER=$(fingerprint new)
check "BM25 answers and metrics_a are identical" test "$(head -n 1 <<<"$AFTER")" == "$(head -n 1 <<<"$BEFORE")"
# the units of an index written by a release have no sketch and are read by their rows; those of this tree's have it: the same number as the normal plan either way
check "sift.approx_count_distinct on the column index is what the normal plan gives" test "$(q new postgres "SET sift.agg_force = on" "SET sift.agg_preagg = on" "SELECT sift.approx_count_distinct(id) || ':' || sift.approx_count_distinct(g) FROM facts")" == \
	"$(q new postgres "SET sift.enable_agg = off" "SELECT sift.approx_count_distinct(id) || ':' || sift.approx_count_distinct(g) FROM facts")"
check "column aggregates are identical and equal the normal plan" test "$(sed -n 2,3p <<<"$AFTER")" == "$(sed -n 2,3p <<<"$BEFORE")"
check "the tables answer as before, by the scan and the normal plan" test "$(sed -n 4,7p <<<"$AFTER")" == "$(sed -n 4,7p <<<"$BEFORE")"
# the cluster runs with sift.table_auto_defrag and sift.col_auto_merge off (scripts/pgtest.sh conf), which the preflight warns about: its session has the shipped values
if [[ -z $OLD_REF ]]; then
	check "the preflight finds nothing to fix" quiet env PGOPTIONS="-c sift.table_auto_defrag=on -c sift.col_auto_merge=on" PGHOST="$(dir new)/sock" PGPORT="$(port new)" PGUSER=postgres "$REPO/scripts/upgrade-check.sh" --strict postgres
else
	# the objects of a release name what the update leaves to REINDEX or VACUUM FULL (warnings), and nothing blocks
	check "the preflight lists nothing that blocks" quiet env PGOPTIONS="-c sift.table_auto_defrag=on -c sift.col_auto_merge=on" PGHOST="$(dir new)/sock" PGPORT="$(port new)" PGUSER=postgres "$REPO/scripts/upgrade-check.sh" postgres
fi

# an index that tracks its changes: pg_upgrade restores the table before its indexes, so the triggers are made again, and they must hear what the upgraded cluster writes
if [[ -z $OLD_REF ]]; then
	check "the tracked index keeps its two internal statement triggers" test "$(q new postgres "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'trk'::regclass AND tgname LIKE 'sift_track_%' AND tgisinternal")" == 2
else skip 1 "an index of a release has no triggers"; fi
check "the tracked index answers as before and as the normal plan" test "$(q new postgres "SELECT trkq_off()")|$(q new postgres "SELECT trkq_on() = trkq_off()")" == "$TRK_BEFORE|t"
q new postgres "UPDATE trk SET id = id + 20000 WHERE id % 7 = 3" "DELETE FROM trk WHERE id % 11 = 0" "INSERT INTO trk SELECT g, g % 7, g FROM generate_series(5001, 5200) g" >/dev/null
check "the tracked index agrees with the normal plan after UPDATE, DELETE and INSERT" test "$(q new postgres "SELECT trkq_on() = trkq_off()")" == t
q new postgres "REINDEX INDEX trk_c" >/dev/null
check "REINDEX gives the tracked index its baseline: complete, heap-free, the answer unchanged" test "$(q new postgres "SELECT trkq_on() = trkq_off()" "SELECT track_changes AND triggers AND complete AND heap_free FROM sift.col_tracking('trk_c')" | tr '\n' ' ')" == "t t "

# the connection: the option, the dependency and the journal (its lineage and records came with the files); the triggers of the tracked parent index are made again, so its log is
# not complete and nothing is vouched until the index is rebuilt
if [[ -z $OLD_REF ]]; then
	check "the connected pair is as before and nothing is vouched while the log is not complete" test "$(q new postgres "$CONNECTION")" == "$(sed 's/ ok$/ log not complete/' <<<"$CONN_BEFORE")"
	q new postgres "UPDATE cdim SET cat = cat + 1 WHERE id = 7" >/dev/null
	check "the journal of the parent index hears the upgraded cluster and the parent index stays under the child" test "$(q new postgres "SELECT sum(records) > $RECS_BEFORE FROM sift.col_connections" 2>&1 | head -n 1)" == t \
		-a "$(q new postgres "DROP INDEX cdim_c" 2>&1 | grep -c 'cannot drop index')$(q new postgres "DROP INDEX cdim2_c" 2>&1 | grep -c 'cannot drop index')" == 11
	q new postgres "REINDEX INDEX cdim_c" "REINDEX INDEX cdim2_c" >/dev/null
	check "REINDEX of the parent indexes makes their journals again: both stamps vouch and the indexes verify" test "$(q new postgres "SELECT string_agg(stamp, ',' ORDER BY ordinal) FROM sift.col_connections")" == ok,ok \
		-a "$(q new postgres "SELECT count(*) FROM (SELECT sift.col_verify('cdim_c', true), sift.col_verify('cdim2_c', true), sift.col_verify('cfact_c', true)) v")" == 1
else skip 3 "an index of a release has no connection"; fi

# the projections: the definitions came with the files, the indexes with the option that names them
if [[ -z $OLD_REF ]]; then
	check "the projections are as before: both listed by the one index, every status ok" test "$(q new postgres "SELECT string_agg(projection || ':' || index || ':' || state, ',' ORDER BY projection) FROM sift.projection_status")" == "pnarrow:pev_wide:ok,pwide:pev_wide:ok"
	check "the guard and the one call work after pg_upgrade" test "$(q new postgres "DELETE FROM sift.projections WHERE name = 'pwide'" 2>&1 | grep -c 'is held by pev_wide')$(q new postgres "SELECT sift.projection_index('pev_other', ARRAY['pnarrow'], '{\"track_changes\": \"off\"}')")" == 1pev_wide
else skip 2 "a release has no projections"; fi

# writes, VACUUM, promotion and merges on what the old cluster wrote
q new postgres "INSERT INTO docs SELECT g, 'alpha3 postupgrade ' || g FROM generate_series(7001, 7300) g" "UPDATE docs SET body = 'beta5 again' WHERE id % 101 = 0" \
	"DELETE FROM docs WHERE id % 83 = 0" "INSERT INTO facts SELECT g, g % 9, g % 1000 / 8.0, 'hi', make_interval(secs => g), md5(g::text)::uuid, now(), '{\"items\": []}' FROM generate_series(5001, 6000) g" \
	"UPDATE facts SET n = n + 1 WHERE id % 50 = 0" "DELETE FROM facts WHERE id % 61 = 0" \
	"INSERT INTO tbl SELECT g, g % 9, 1, 'lo' FROM generate_series(5001, 6000) g" "UPDATE tbl SET n = n + 2 WHERE id % 30 = 0" "DELETE FROM tbl WHERE id % 31 = 0" \
	"INSERT INTO tbl_ts SELECT g, g % 9, 1 FROM generate_series(5001, 6000) g" "DELETE FROM tbl_ts WHERE id % 31 = 0" >/dev/null
check "the upgraded cluster takes writes of the three kinds" test $? -eq 0
q new postgres "VACUUM docs" "VACUUM facts" "VACUUM tbl" "VACUUM tbl_ts" "ANALYZE" >/dev/null
check "VACUUM and ANALYZE pass" test $? -eq 0
q new postgres "SELECT sift.promote('docs_idx') >= 0, sift.merge('docs_idx', true) >= 0, sift.merge('docs_ts_idx', true) >= 0, sift.col_promote('facts_col') >= 0, sift.col_merge('facts_col', true) >= 0, sift.col_merge('facts_json', true) >= 0" \
	"SELECT sift.table_maintain('tbl') IS NOT NULL, sift.table_maintain('tbl_ts') IS NOT NULL" >/dev/null
check "promotion, merges and sift.table_maintain pass" test $? -eq 0
check "sift.verify and sift.col_verify pass after the writes and merges" quiet q new postgres "$VERIFY_SQL"
check "the BM25 index agrees with a sequential scan" test "$(q new postgres "SET sift.enable_scan = off" "SET sift.enable_count = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" \
	"SELECT count(*) || ':' || sum(id) FROM docs WHERE body @@> 'alpha3'")" == "$(q new postgres "SELECT count(*) || ':' || sum(id) FROM docs WHERE body @@> 'alpha3'")"
check "the column indexes agree with the normal plan after the writes" test "$(q new postgres "SELECT colq_on() = colq_off()")" == t
check "the tables agree with the normal plan after the writes" test "$(q new postgres "SELECT tblq_on('tbl') = tblq_off('tbl') AND tblq_on('tbl_ts') = tblq_off('tbl_ts')")" == t
check "the heap twin of the table counts what the table counts" test "$(q new postgres "CREATE TEMP TABLE twin AS SELECT * FROM tbl" "SELECT tblq_off('tbl') = tblq_off('pg_temp.twin')" | tail -n 1)" == t
finish
