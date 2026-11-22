#!/usr/bin/env bash
# tap: pooled
# The first statement of a session that has not preloaded the library is a COPY .. FROM: the library loads while the COPY runs, and what it decides about that statement must not open
# the target (a relation of the sift table access method calls the handler in the library, which is still loading). A fresh psql is a fresh backend: nothing is loaded before the COPY.
. "$(dirname "$0")/lib.bash"

plan 5
node_init cf 28 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start cf || bail "start failed"
D=$(dir cf)
sql cf "CREATE EXTENSION pg_sift" \
	"CREATE TABLE src AS SELECT g AS id, g % 7 AS grp, g % 11 AS v FROM generate_series(1, 2000) g" \
	"COPY src TO '$D/load.csv' CSV" \
	"CREATE TABLE tb (id int, grp int, v int) USING sift_table" \
	"CREATE FUNCTION nop() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN RETURN NULL; END \$\$" >/dev/null || bail "setup failed"
for t in pt pb ps; do
	sql cf "CREATE TABLE $t (id int, grp int, v int) PARTITION BY HASH (id)" \
		"CREATE TABLE ${t}0 PARTITION OF $t FOR VALUES WITH (MODULUS 2, REMAINDER 0)" \
		"CREATE TABLE ${t}1 PARTITION OF $t FOR VALUES WITH (MODULUS 2, REMAINDER 1)" \
		"CREATE INDEX ${t}0_c ON ${t}0 USING sift_col (id, grp, v)" "CREATE INDEX ${t}1_c ON ${t}1 USING sift_col (id, grp, v)" >/dev/null || bail "table $t failed"
done
sql cf "CREATE TRIGGER before_copy BEFORE INSERT ON ps FOR EACH STATEMENT EXECUTE FUNCTION nop()" >/dev/null || bail "trigger failed"

# load TABLE [BEGIN]: one fresh backend whose first statement is the COPY (after BEGIN in a transaction block); prints what the table holds
load() {
	{ [[ -z ${2:-} ]] || echo "$2;"; echo "COPY $1 FROM '$D/load.csv' CSV;"; echo "SELECT count(*) || ':' || sum(v) FROM $1;"; } | sql cf 2>&1 | tail -1
}
want=$(sql cf "SELECT count(*) || ':' || sum(v) FROM src" | tail -1)

check "a sift_table" test "$(load tb)" == "$want"
check "a partitioned table of sift_col partitions" test "$(load pt)" == "$want"
check "the same in a transaction block" test "$(load pb BEGIN)" == "$want"
check "a table with a BEFORE STATEMENT trigger" test "$(load ps)" == "$want"

check "the index of a partition verifies" test -z "$(sql cf "SELECT sift.verify('pt0_c', true)" 2>&1)"
