#!/usr/bin/env bash
# The apply worker of a logical subscriber changes rows without firing statement triggers, so a scope copy of a table it writes would miss the changes: sift.scope_create refuses a
# fact table or a scope relation that is the target of a subscription, and builds the copy once the subscription is gone. Tables that only the publisher has a subscription for
# are no target: the copy of the publisher's tables is made and exact there. A subscription created after the copy leaves it behind (the apply worker fires no trigger):
# sift.scope_verify names the cause.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_scope.bash"

plan 10
node_init p 53 "wal_level = logical
max_wal_senders = 5
max_replication_slots = 5
autovacuum = off
max_logical_replication_workers = 4
max_worker_processes = 16"
node_start p || bail "publisher start failed"
node_init s 54 "wal_level = logical
max_replication_slots = 5
autovacuum = off
max_logical_replication_workers = 4
max_worker_processes = 16"
node_start s || bail "subscriber start failed"
export PGOPTIONS="-c client_min_messages=warning"

TABLES="CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL, note text)
CREATE INDEX ON facts (dim_a_id)
CREATE TABLE pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id))"
while IFS= read -r q; do sql p "$q" >/dev/null && sql s "$q" >/dev/null || bail "ddl failed: $q"; done <<<"CREATE EXTENSION pg_sift
$TABLES"
sql p "INSERT INTO pairs SELECT e, 1 + e % 3 FROM generate_series(1, 20) e" "INSERT INTO facts SELECT i, 1 + i % 20, now(), i % 5, i, 'n' FROM generate_series(1, 200) i" \
	"CREATE PUBLICATION pf FOR TABLE facts" "CREATE PUBLICATION pp FOR TABLE pairs" >/dev/null || bail "publisher setup failed"
COPY="SELECT sift.scope_create('c', 'facts', 'dim_a_id', 'pairs', 'scope_id')"
refused() { local msg; msg=$(sql s "$COPY" 2>&1) && return 1; [[ $msg == *'logical replication subscription'* ]]; }
copies() { sql s "SELECT count(*) FROM sift.scope_copies"; }

sql s "CREATE SUBSCRIPTION sf CONNECTION '$(conninfo p)' PUBLICATION pf" >/dev/null || bail "subscribe failed"
wait_for '[[ $(sql s "SELECT count(*) FROM facts") == 200 ]]' 120
check "a fact table that is the target of a subscription is refused" refused
sql s "DROP SUBSCRIPTION sf" >/dev/null
sql s "CREATE SUBSCRIPTION sp CONNECTION '$(conninfo p)' PUBLICATION pp" >/dev/null || bail "subscribe failed"
wait_for '[[ $(sql s "SELECT count(*) FROM pairs") == 20 ]]' 120
check "a scope relation that is the target of a subscription is refused" refused
check "... and nothing of the copy is left" test "$(copies)|$(sql s "SELECT count(*) FROM pg_class WHERE relname LIKE 'c%' AND relnamespace = 'public'::regnamespace AND relname NOT IN ('facts', 'pairs', 'facts_pkey', 'pairs_pkey')")" == "0|0"
sql s "DROP SUBSCRIPTION sp" >/dev/null
check "without the subscription the copy is made" test "$(sql s "$COPY" | tail -n 1)" -ge 1
check "... and a change of the tables keeps it exact" test "$(sql s "INSERT INTO facts VALUES (900001, 2, now(), 1, 1, 'x')" "INSERT INTO pairs VALUES (2, 7)" "SELECT count(*) FROM sift.scope_verify('c')" | tail -n 1)" == 0

# a copy on the publisher, whose tables are published and have no subscription: exact under the changes
sql p "SELECT sift.scope_create('c', 'facts', 'dim_a_id', 'pairs', 'scope_id')" >/dev/null
sql p "INSERT INTO facts VALUES (900001, 2, now(), 1, 1, 'x')" "INSERT INTO pairs VALUES (2, 7)" "UPDATE facts SET amount = amount + 1 WHERE id % 5 = 0" >/dev/null
check "the publisher's copy of published tables stays exact" test "$(sql p "SELECT count(*) FROM sift.scope_verify('c')")" == 0
check "the subscriber's catalog has no subscription left" test "$(sql s "SELECT count(*) FROM pg_subscription_rel")" == 0

# a subscription made after the copy: the apply worker inserts a fact and the copy misses it
sql s "CREATE SUBSCRIPTION sl CONNECTION '$(conninfo p)' PUBLICATION pf WITH (copy_data = false)" >/dev/null || bail "subscribe failed"
sql p "INSERT INTO facts VALUES (900100, 2, now(), 1, 1, 'x')" >/dev/null
wait_for '[[ $(sql s "SELECT count(*) FROM facts WHERE id = 900100") == 1 ]]' 120
verify() { sql s "SELECT problem || ' ' || coalesce(row_key->>'table', row_key::text) FROM sift.scope_verify('c') ORDER BY 1"; }
check "scope_verify names a subscription created after the copy as the cause" test "$(verify | grep -c '^logical replication target public.facts$')" == 1
check "... and the copy misses the fact the apply worker inserted" test "$(verify | grep -c '^missing ')" -ge 1
sql s "DROP SUBSCRIPTION sl" >/dev/null
check "without the subscription scope_verify names no cause" test "$(verify | grep -c '^logical replication target')" == 0
finish
