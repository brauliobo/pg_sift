#!/usr/bin/env bash
# With sift.on_corruption = quarantine (the default) the commit writes the tombstones a statement held through the guard of the index, which runs in a subtransaction of its own. The start of that
# subtransaction must not flush the held rows again: an UPDATE of the parent table of a connected index in a new session commits, and the journal of the parent holds the changed keys.
. "$(dirname "$0")/lib.bash"

plan 3
node_init qh 29 "autovacuum = off
sift.on_corruption = quarantine"
node_start qh || bail "start failed"
sql qh "CREATE EXTENSION pg_sift" \
	"CREATE TABLE cdim (id int PRIMARY KEY, cat int NOT NULL, label text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO cdim SELECT g, g % 10, 'l' || g FROM generate_series(1, 500) g" \
	"CREATE TABLE cfact (id int PRIMARY KEY, dim_id int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO cfact SELECT g, 1 + g % 500, g FROM generate_series(1, 2000) g" \
	"CREATE INDEX cdim_c ON cdim USING sift_col (id, cat, label)" \
	"CREATE INDEX cfact_c ON cfact USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> cdim(id)')" >/dev/null 2>&1 || bail "setup failed"

out=$(sql qh "UPDATE cdim SET cat = cat + 1 WHERE id % 50 = 0" 2>&1)
check "an UPDATE of the parent of a connected index commits in a new session" test -z "$out"
check "the rows are those of the statement" test "$(sql qh 'SELECT count(*) FROM cdim WHERE cat = 1' | tail -1)" == "60"
check "the index verifies" test -z "$(sql qh "SELECT sift.verify('cdim_c', true)" 2>&1)"
