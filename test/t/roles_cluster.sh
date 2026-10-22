#!/usr/bin/env bash
# What the regress tests of roles_*.sql cannot do as an ordinary role: it connects as one. The owner of a schema with every kind of sift object
# (sift and sift_col indexes, a sift_table, partitions) takes a pg_dump of the schema and restores it with pg_restore into another database, moves
# the tables and indexes to a tablespace it was given, and publishes tables with wal_level = logical while pg_sift is not preloaded (a published
# sift_table is refused with the error of the extension, a heap table is not). A role without the privileges is refused each of them.
. "$(dirname "$0")/lib.bash"

plan 22
N=rl
node_init $N 0 "wal_level = logical"
node_start $N || bail "start"
TS=$(dir $N)/ts
mkdir -p "$TS"

# q DB ROLE QUERY...: as the role, stops at the first error; qe: the same, the error text on stdout
q() { local db=$1 role=$2; shift 2; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir $N)/sock" -p "$(port $N)" -U "$role" -d "$db" "${@/#/--command=}"; }
qe() { q "$@" 2>&1 | tr '\n' ' ' | sed 's/ *$//'; }
refused() { local out; out=$(qe "$@"); [[ $out == *"$REFUSAL"* ]] || { diag "got: $out"; return 1; }; }

q postgres postgres "CREATE ROLE rl_owner LOGIN" "CREATE ROLE rl_other LOGIN" "CREATE DATABASE rl_src OWNER postgres" "CREATE DATABASE rl_dst OWNER postgres" \
	"CREATE TABLESPACE rl_ts LOCATION '$TS'" "GRANT CREATE ON TABLESPACE rl_ts TO rl_owner" >/dev/null || bail "roles"
for db in rl_src rl_dst; do
	q $db postgres "CREATE EXTENSION pg_sift" "GRANT CREATE ON DATABASE $db TO rl_owner" >/dev/null || bail "extension in $db"
done
q rl_src postgres "CREATE SCHEMA app AUTHORIZATION rl_owner" "GRANT USAGE, CREATE ON SCHEMA app TO rl_other" >/dev/null || bail "schema"
q rl_src rl_owner "
CREATE TABLE app.docs (id int PRIMARY KEY, body text, g int, v float8);
INSERT INTO app.docs SELECT i, 'alpha beta ' || i, i % 3, i FROM generate_series(1, 600) i;
CREATE INDEX docs_idx ON app.docs USING sift (body);
CREATE INDEX docs_col ON app.docs USING sift_col (id, g, v);
CREATE TABLE app.facts (id int, g int, val float8, doc jsonb) USING sift_table;
INSERT INTO app.facts SELECT i, i % 3, i, jsonb_build_object('a', i % 5) FROM generate_series(1, 600) i;
CREATE TABLE app.part (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE app.part_1 PARTITION OF app.part FOR VALUES FROM (0) TO (300);
CREATE TABLE app.part_2 PARTITION OF app.part FOR VALUES FROM (300) TO (600);
INSERT INTO app.part SELECT i, 'alpha part ' || i FROM generate_series(0, 599) i;
CREATE INDEX part_idx ON app.part USING sift (body);
CREATE TABLE app.plain (id int, t text);
INSERT INTO app.plain VALUES (1, 'x')" >/dev/null || bail "data"

# the answers of a database, as the owner: the BM25 index, the column index, the sift_table, the partitions
ANSWERS="SELECT (SELECT count(*) || ':' || sum(id) FROM app.docs WHERE body @@> 'alpha') || ' ' ||
	(SELECT string_agg(g || '=' || n || '/' || s, ',' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM app.docs GROUP BY g) x) || ' ' ||
	(SELECT string_agg(g || '=' || n || '/' || s, ',' ORDER BY g) FROM (SELECT g, count(*) n, sum(val) s FROM app.facts GROUP BY g) x) || ' ' ||
	(SELECT count(*) FROM app.part WHERE body @@> 'part')"
WANT=$(q rl_src rl_owner "SET enable_seqscan = off" "SET sift.enable_agg = on" "$ANSWERS" | tail -1)
PLAIN=$(q rl_src rl_owner "SET sift.enable_scan = off" "SET sift.enable_count = off" "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "$ANSWERS" | tail -1)
check "the owner reads every kind of sift object in the source database, as the plain plans do" test "$WANT" == "$PLAIN"

# --- pg_dump of the schema by its owner, pg_restore by its owner
DUMP=$(dir $N)/app.dump
pgd() { PGOPTIONS="-c client_min_messages=warning" "$@"; }
check "pg_dump -Fc -n app as the owner" pgd pg_dump -h "$(dir $N)/sock" -p "$(port $N)" -U rl_owner -d rl_src -n app -Fc -f "$DUMP"
check "the dump holds the three access methods" bash -c "pg_restore -f - '$DUMP' | grep -c -E 'USING (sift|sift_col) |sift_table' | grep -q '^[3-9]'"
check "pg_restore as the owner into a database with the extension restores the schema and the data" pgd pg_restore -h "$(dir $N)/sock" -p "$(port $N)" -U rl_owner -d rl_dst --no-owner -e "$DUMP"
GOT=$(q rl_dst rl_owner "SET enable_seqscan = off" "SET sift.enable_agg = on" "$ANSWERS" | tail -1)
check "the restored database answers as the source" test "$GOT" == "$WANT"
check "the indexes of the restored database verify" test "$(q rl_dst postgres "SELECT sift.verify('app.docs_idx', true)" "SELECT sift.col_verify('app.docs_col', true)" "SELECT sift.verify('app.part_1_body_idx', true)" 2>&1 | grep -c ERROR)" == 0
check "the owner of the restored schema drops it (tables, indexes, the partitions, the sift_table)" \
	test "$(qe rl_dst rl_owner "DROP TABLE app.part, app.facts, app.docs, app.plain" "SELECT count(*) FROM pg_class WHERE relnamespace = 'app'::regnamespace")" == 0
check "a role that owns nothing cannot dump what it may not read" \
	bash -c "! pg_dump -h '$(dir $N)/sock' -p '$(port $N)' -U rl_other -d rl_src -n app -f /dev/null 2>/dev/null"

# --- a tablespace given to the owner
check "the owner moves a heap table, a BM25 index, a sift_col index and a sift_table to the tablespace" \
	test "$(qe rl_src rl_owner "ALTER TABLE app.docs SET TABLESPACE rl_ts" "ALTER INDEX app.docs_idx SET TABLESPACE rl_ts" "ALTER INDEX app.docs_col SET TABLESPACE rl_ts" "ALTER TABLE app.facts SET TABLESPACE rl_ts" "SELECT count(*) FROM pg_class WHERE relname IN ('docs', 'docs_idx', 'docs_col', 'facts') AND reltablespace = (SELECT oid FROM pg_tablespace WHERE spcname = 'rl_ts')")" == 4
check "the answers are the same in the tablespace" test "$(q rl_src rl_owner "SET enable_seqscan = off" "SET sift.enable_agg = on" "$ANSWERS" | tail -1)" == "$WANT"
check "the owner creates tables and indexes there" test "$(qe rl_src rl_owner "CREATE TABLE app.t2 (id int, body text) TABLESPACE rl_ts" "CREATE INDEX t2_idx ON app.t2 USING sift (body) TABLESPACE rl_ts" "CREATE INDEX t2_col ON app.t2 USING sift_col (id) TABLESPACE rl_ts" "CREATE TABLE app.t3 (id int) USING sift_table TABLESPACE rl_ts" "DROP TABLE app.t2, app.t3")" == ""
REFUSAL="permission denied for tablespace rl_ts"
check "a role without CREATE on the tablespace is refused" refused rl_src rl_other "CREATE TABLE app.t4 (id int) TABLESPACE rl_ts"
REFUSAL="must be owner of table docs"
check "and cannot move the owner's table" refused rl_src rl_other "ALTER TABLE app.docs SET TABLESPACE pg_default"
check "the owner moves them back" test "$(qe rl_src rl_owner "ALTER TABLE app.docs SET TABLESPACE pg_default" "ALTER INDEX app.docs_idx SET TABLESPACE pg_default" "ALTER INDEX app.docs_col SET TABLESPACE pg_default" "ALTER TABLE app.facts SET TABLESPACE pg_default")" == ""

# --- logical replication: wal_level = logical and the library not preloaded
REFUSAL='sift_table table "app.facts" is published and wal_level is "logical", but pg_sift is not in shared_preload_libraries'
check "a publication of a sift_table is refused with the error of the extension" refused rl_src rl_owner "CREATE PUBLICATION rl_pub FOR TABLE app.facts"
check "so is ALTER PUBLICATION ADD TABLE" refused rl_src rl_owner "CREATE PUBLICATION rl_pub FOR TABLE app.plain" "ALTER PUBLICATION rl_pub ADD TABLE app.facts"
check "the failed ones left only the publication of the heap table" test "$(q rl_src postgres "SELECT string_agg(pubname, ',') FROM pg_publication")" == rl_pub
check "a heap table with sift indexes is published" test "$(qe rl_src rl_owner "CREATE PUBLICATION rl_pub2 FOR TABLE app.part_1 WITH (publish = 'insert')")" == ""
REFUSAL="must be superuser to create FOR TABLES IN SCHEMA publication"
check "FOR TABLES IN SCHEMA stays for superusers" refused rl_src rl_owner "CREATE PUBLICATION rl_pub3 FOR TABLES IN SCHEMA app"
check "the owner drops the publications" test "$(qe rl_src rl_owner "DROP PUBLICATION rl_pub, rl_pub2")" == ""

# --- nothing is left: the objects, the tablespace, the roles
q rl_src postgres "DROP SCHEMA app CASCADE" >/dev/null
check "the tablespace is dropped once its tables are" test "$(qe postgres postgres "DROP DATABASE rl_dst" "DROP DATABASE rl_src" "DROP TABLESPACE rl_ts" "DROP ROLE rl_owner, rl_other")" == ""
check "no database object of the roles is left" test "$(q postgres postgres "SELECT count(*) FROM pg_roles WHERE rolname LIKE 'rl\_%'")" == 0

finish
