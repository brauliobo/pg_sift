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
q() { local n=$1 db=$2; shift 2; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$n")/sock" -p "$(port "$n")" -U postgres -d "$db" "${@/#/--command=}"; }
# qs NODE DB: the script on stdin, one statement at a time (CREATE TABLESPACE cannot share a transaction)
qs() { PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$1")/sock" -p "$(port "$1")" -U postgres -d "$2" -f -; }
quiet() { "$@" >/dev/null; }
PG_BINS="postgres initdb pg_ctl pg_upgrade vacuumdb pg_resetwal pg_controldata pg_dump pg_dumpall pg_restore pg_checksums pg_config psql"

# overlay PG_CONFIG DEST ROOT: the server of PG_CONFIG at its absolute paths under DEST, pg_sift of the install ROOT in it
overlay() {
	local bin lib share f
	bin=$("$1" --bindir); lib=$("$1" --pkglibdir); share=$("$1" --sharedir)
	mkdir -p "$2$bin" "$2$lib" "$2$share/extension"
	for f in $PG_BINS; do cp "$bin/$f" "$2$bin/"; done
	for f in "$lib"/*; do ln -s "$f" "$2$lib/"; done
	for f in "$share"/*; do [[ $f == */extension ]] || ln -s "$f" "$2$share/"; done
	for f in "$share"/extension/*; do ln -s "$f" "$2$share/extension/"; done
	[[ -f $3$lib/pg_sift.so ]] || return 0   # PostgreSQL 17 has no staged root: sift is in the server's own directories
	rm -f "$2$lib/pg_sift.so" "$2$share"/extension/pg_sift*
	cp "$3$lib/pg_sift.so" "$2$lib/"
	cp "$3$share"/extension/pg_sift* "$2$share/extension/"
}

# build_old PG_CONFIG ROOT: pg_sift for another major, built in a copy of this tree (the tree under test keeps its objects)
build_old() {
	local src=$TAPDIR/old-src
	mkdir -p "$src"
	tar -C "$REPO" --exclude=.git --exclude='*.o' --exclude='*.d' --exclude='*.so' --exclude='*.bc' --exclude=.build-flavor --exclude=bench/out -cf - . | tar -x -C "$src"
	{ nice make -C "$src" -j4 USE_PGXS=1 PG_CONFIG="$1" SIFT_TESTING= && make -C "$src" USE_PGXS=1 PG_CONFIG="$1" SIFT_TESTING= DESTDIR="$2" install; } \
		>"$TAPDIR/old-build.log" 2>&1
}

NEWCFG=$(command -v pg_config)
OLDBIN=${SIFT_PG_OLD_BIN:-}
if [[ -z $OLDBIN ]]; then
	for c in "/usr/lib/postgresql/$((PG_MAJOR - 1))/bin" "/usr/pgsql-$((PG_MAJOR - 1))/bin"; do
		[[ -x $c/pg_config ]] && { OLDBIN=$c; break; }
	done
fi
OLDCFG=${OLDBIN:+$OLDBIN/pg_config}
OLDCFG=${OLDCFG:-$NEWCFG}
OLD_MAJOR=$("$OLDCFG" --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
# a server image can carry a major pg_sift does not support (the Makefile's PG_MIN_MAJOR): the upgrade then runs within the major
if ((OLD_MAJOR < $(sed -n 's/^PG_MIN_MAJOR *:\?= *\([0-9]*\).*/\1/p' "$REPO/Makefile" | head -n 1))); then OLDCFG=$NEWCFG; OLD_MAJOR=$PG_MAJOR; fi

TS=$TAPDIR/tablespace
OV=$TAPDIR/overlay
trap 'pg_ctl -D "$(dir old)/data" -m immediate stop >/dev/null 2>&1; cleanup; ((NFAIL)) || rm -rf "$(dir old)" "$OV" "$TS" "$TAPDIR/old-src"' EXIT
plan 19
rm -rf "$TS" "$OV"
mkdir -p "$TS" "$OV"
diag "pg_upgrade from PostgreSQL $OLD_MAJOR to $PG_MAJOR"

# the servers: this major from the staged install, the old one from the same or a build of this tree for its major
overlay "$NEWCFG" "$OV/new" "$ROOT"
if ((OLD_MAJOR == PG_MAJOR)); then
	overlay "$OLDCFG" "$OV/old" "$ROOT"
else
	build_old "$OLDCFG" "$OV/oldroot" || { tail -n 20 "$TAPDIR/old-build.log"; bail "pg_sift does not build for PostgreSQL $OLD_MAJOR"; }
	overlay "$OLDCFG" "$OV/old" "$OV/oldroot"
fi
OLDB=$OV/old$("$OLDCFG" --bindir)
NEWB=$OV/new$("$NEWCFG" --bindir)
PATH=$NEWB:$PATH

# initdb_node NAME OFFSET BINDIR: the cluster every test starts from, with data checksums on both sides
initdb_node() {
	local d
	d=$(dir "$1")
	mkdir -p "$d/sock"
	echo "$2" >"$d/offset"
	"$3/initdb" -k --no-sync -D "$d/data" -A trust -U postgres -E UTF8 --no-locale --no-instructions --wal-segsize=1 >"$d/initdb.log" 2>&1 || bail "initdb $1 failed"
	node_conf "$1" "port = $(port "$1")
listen_addresses = ''
unix_socket_directories = '$d/sock'
fsync = off
shared_buffers = 32MB
max_connections = 30"
}
initdb_node old 30 "$OLDB"
initdb_node new 31 "$NEWB"
NODES+=(new)
node_conf new "$("$REPO/scripts/pgtest.sh" conf)"
LD_PRELOAD=${SIFT_PRELOAD:-} "$OLDB/pg_ctl" -D "$(dir old)/data" -l "$(dir old)/postgres.log" -w -t 120 start >/dev/null 2>&1 || bail "the old cluster does not start"

# a tablespace needs two catalog versions (pg_upgrade refuses the same one)
TSDDL=; TSC=
if ((OLD_MAJOR != PG_MAJOR)); then TSDDL="CREATE TABLESPACE ts1 LOCATION '$TS';"; TSC="TABLESPACE ts1"; fi

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
CREATE INDEX facts_col ON facts USING sift_col (id, g, n, t, w, u, ts) WITH (cube_minmax = true, order_columns = 'g, n', key_columns = 'g');
CREATE INDEX facts_json ON facts USING sift_col (id, doc) WITH (paths = 'doc.items[]{category text, qty int4}') $TSC;
CREATE TABLE tbl (id int, g int, n numeric(12,2), t tone) USING sift_table;
INSERT INTO tbl SELECT g, g % 9, g % 1000 / 8.0, (ARRAY['lo', 'mid', 'hi'])[g % 3 + 1]::tone FROM generate_series(1, 5000) g;
UPDATE tbl SET n = n + 1 WHERE id % 20 = 0;
DELETE FROM tbl WHERE id % 25 = 0;
CREATE TABLE tbl_ts (id int, g int, n int) USING sift_table $TSC;
INSERT INTO tbl_ts SELECT g, g % 9, g % 100 FROM generate_series(1, 5000) g;
VACUUM tbl_ts;
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
VERIFY_SQL="SELECT sift.verify('docs_idx', true), sift.verify('docs_ts_idx', true), sift.col_verify('facts_col', true), sift.col_verify('facts_json', true)"
fingerprint() { q "$1" postgres "SELECT bm25q()" "SELECT colq_on() = colq_off()" "SELECT colq_off()" "SELECT tblq_off('tbl')" "SELECT tblq_on('tbl') = tblq_off('tbl')" \
	"SELECT tblq_off('tbl_ts')" "SELECT tblq_on('tbl_ts') = tblq_off('tbl_ts')"; }
VERSIONS=$(q old postgres "SELECT extversion || ' ' || sift.version() FROM pg_extension WHERE extname = 'pg_sift'")
BEFORE=$(fingerprint old)
check "the old cluster agrees with itself: the scans equal the normal plans" test "$(grep -c '^t$' <<<"$BEFORE")" == 3
FILES_BEFORE=$(q old postgres "SELECT string_agg(relname || '=' || pg_relation_filepath(oid), ',' ORDER BY relname) FROM pg_class WHERE relname IN ('docs_ts_idx', 'facts_json', 'tbl_ts')")
if [[ -n $TSC ]]; then
	check "the tablespace holds an index of each access method and a table" test "$(grep -o 'pg_tblspc' <<<"$FILES_BEFORE" | wc -l)" == 3
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
	diag "$(tail -n 20 "$(dir new)/pg_upgrade.log"; tail -n 8 "$(dir new)"/data/pg_upgrade_output.d/*/log/pg_upgrade_server.log 2>/dev/null)"
	bail "pg_upgrade failed"
fi
node_start new || bail "the upgraded cluster does not start"

check "the extension and the library are the ones of before" test "$(q new postgres "SELECT extversion || ' ' || sift.version() FROM pg_extension WHERE extname = 'pg_sift'")" == "$VERSIONS"
if [[ -n $TSC ]]; then
	check "the relations of the tablespace are under it, with the same file names" test "$(q new postgres "SELECT string_agg(relname || '=' || pg_relation_filepath(oid), ',' ORDER BY relname) FROM pg_class WHERE relname IN ('docs_ts_idx', 'facts_json', 'tbl_ts')" |
	sed 's#PG_[0-9]*_[0-9]*#V#g')" == "$(sed 's#PG_[0-9]*_[0-9]*#V#g' <<<"$FILES_BEFORE")"
else skip 1 "no tablespace"; fi
check "sift.verify and sift.col_verify pass on every index" quiet q new postgres "$VERIFY_SQL"
AFTER=$(fingerprint new)
check "BM25 answers and metrics_a are identical" test "$(head -n 1 <<<"$AFTER")" == "$(head -n 1 <<<"$BEFORE")"
check "column aggregates are identical and equal the normal plan" test "$(sed -n 2,3p <<<"$AFTER")" == "$(sed -n 2,3p <<<"$BEFORE")"
check "the tables answer as before, by the scan and the normal plan" test "$(sed -n 4,7p <<<"$AFTER")" == "$(sed -n 4,7p <<<"$BEFORE")"
# the cluster runs with sift.table_auto_defrag and sift.col_auto_merge off (scripts/pgtest.sh conf), which the preflight warns about: its session has the shipped values
check "the preflight finds nothing to fix" quiet env PGOPTIONS="-c sift.table_auto_defrag=on -c sift.col_auto_merge=on" PGHOST="$(dir new)/sock" PGPORT="$(port new)" PGUSER=postgres "$REPO/scripts/upgrade-check.sh" --strict postgres

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
