#!/usr/bin/env bash
# A scope keyed by a text, uuid or date column of an index in format major 2 on a primary, a hot standby, a primary that crashed and a promoted standby: the cubes of the summary are cut by the codes
# of the scope key store of the key column (text, uuid) or by the value itself (date), and every one of those nodes answers the scoped statements as the plain join does. The primary adds a key
# that no summary has, moves and drops pairs, inserts, updates and deletes facts, and vacuums; the standby replays the stores of the codes with the pages of the summary.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 15

p=f2k_primary
s=f2k_standby
node_init "$p" 6 "wal_level = replica
max_wal_senders = 4
autovacuum = off
sift.async_maintenance = off"
node_start "$p" || bail "primary start failed"
sql "$p" "CREATE EXTENSION pg_sift" "SET sift.test_create_format1 = off" \
	"CREATE TABLE events (id int PRIMARY KEY, member int NOT NULL, created_at timestamptz NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events SELECT i, 1 + i % 60, timestamptz '2026-01-01' + i * interval '97 minutes', i % 101 FROM generate_series(1, 4000) i" \
	"CREATE TABLE pairs_text (member int NOT NULL, key text NOT NULL)" "CREATE UNIQUE INDEX pairs_text_u ON pairs_text (member, key)" \
	"CREATE TABLE pairs_uuid (member int NOT NULL, key uuid NOT NULL)" "CREATE UNIQUE INDEX pairs_uuid_u ON pairs_uuid (member, key)" \
	"CREATE TABLE pairs_date (member int NOT NULL, key date NOT NULL)" "CREATE UNIQUE INDEX pairs_date_u ON pairs_date (member, key)" \
	"INSERT INTO pairs_text SELECT m, 'tenant-' || ((m + j) % 9) FROM generate_series(1, 60) m, generate_series(0, 2) j WHERE j < m % 4" \
	"INSERT INTO pairs_uuid SELECT m, md5('k' || ((m + j) % 9))::uuid FROM generate_series(1, 60) m, generate_series(0, 2) j WHERE j < m % 4" \
	"INSERT INTO pairs_date SELECT m, date '2026-01-01' + ((m + j) % 9) FROM generate_series(1, 60) m, generate_series(0, 2) j WHERE j < m % 4" \
	"CREATE INDEX events_text ON events USING sift_col (member, created_at, amount) WITH (scope = 'k: member -> pairs_text(member, key)', track_changes = 'ctid')" \
	"CREATE INDEX events_uuid ON events USING sift_col (member, created_at, amount) WITH (scope = 'k: member -> pairs_uuid(member, key)', track_changes = 'ctid')" \
	"CREATE INDEX events_date ON events USING sift_col (member, created_at, amount) WITH (scope = 'k: member -> pairs_date(member, key)', track_changes = 'ctid')" \
	"VACUUM ANALYZE events" >/dev/null || bail "setup failed"
check "the three indexes are in format major 2 with a summary" test "$(sql "$p" "SELECT string_agg(format_major || ':' || scopes, ',' ORDER BY relation::text) FROM sift.status() WHERE relation::text LIKE 'events\_%'")" == "2:1,2:1,2:1"
node_clone "$p" "$s" 7 "hot_standby_feedback = on"
node_start "$s" || bail "standby start failed"
check "the standby catches up" catchup "$p" "$s"

# the scoped statements of the three key types: the node's answer with the aggregate on, then the join's (aggregate off); one string each
statements() {
	local k
	for k in "p.key = 'tenant-3'" "p.key = 'tenant-9'" "p.key IN ('tenant-0', 'tenant-8', 'absent')" "p.key = 'tenant-1' AND e.created_at >= '2026-02-01'"; do
		echo "SELECT count(*), sum(e.amount) FROM events e JOIN pairs_text p ON p.member = e.member WHERE $k"
	done
	echo "SELECT p.key, count(*), sum(e.amount) FROM events e JOIN pairs_text p ON p.member = e.member GROUP BY p.key"
	echo "SELECT count(*), sum(e.amount) FROM events e JOIN pairs_uuid p ON p.member = e.member WHERE p.key = md5('k3')::uuid"
	echo "SELECT count(*), sum(e.amount) FROM events e JOIN pairs_uuid p ON p.member = e.member WHERE p.key = md5('k9')::uuid"
	echo "SELECT count(*), sum(e.amount) FROM events e JOIN pairs_date p ON p.member = e.member WHERE p.key = date '2026-01-04'"
	echo "SELECT count(*), sum(e.amount) FROM events e JOIN pairs_date p ON p.member = e.member WHERE p.key = date '2026-01-10'"
	echo "SELECT e.member, e.created_at, e.amount FROM events e JOIN pairs_text p ON p.member = e.member WHERE p.key = 'tenant-2' ORDER BY e.created_at DESC LIMIT 5"
}
answers() { # NODE ON|OFF
	local q
	while IFS= read -r q; do
		sql "$1" "SET sift.enable_agg = $2" "SET max_parallel_workers_per_gather = 0" "SELECT coalesce(string_agg(r::text, '|' ORDER BY r::text), '') FROM ($q) r" 2>&1
	done < <(statements)
}
agree() { # NODE DESCRIPTION
	local on off
	on=$(answers "$1" on)
	off=$(answers "$1" off)
	out=$on
	check "$2: no statement fails" test "$(grep -c ERROR <<<"$on$off")" == 0
	check "$2: the summary answers what the join does" test "$on" == "$off"
}

agree "$s" "the standby after the build"
sql "$p" "SET sift.test_create_format1 = off" \
	"INSERT INTO pairs_text SELECT m, 'tenant-9' FROM generate_series(1, 12) m" "INSERT INTO pairs_uuid SELECT m, md5('k9')::uuid FROM generate_series(1, 12) m" \
	"INSERT INTO pairs_date SELECT m, date '2026-01-10' FROM generate_series(1, 12) m" \
	"DELETE FROM pairs_text WHERE key = 'tenant-0'" "DELETE FROM pairs_uuid WHERE key = md5('k0')::uuid" "UPDATE pairs_text SET key = 'tenant-1' WHERE key = 'tenant-4' AND member % 2 = 0" \
	"INSERT INTO events SELECT 10000 + i, 1 + i % 60, timestamptz '2026-09-01' + i * interval '11 minutes', i % 13 FROM generate_series(1, 500) i" \
	"DELETE FROM events WHERE id % 37 = 0" "UPDATE events SET amount = amount + 500 WHERE id % 41 = 0" >/dev/null || bail "primary writes failed"
check "the standby catches up with the pairs and the facts" catchup "$p" "$s"
agree "$s" "the standby after the writes (no summary has the new keys)"
sql "$p" "VACUUM events" >/dev/null || bail "vacuum failed"
check "the standby catches up with the vacuum" catchup "$p" "$s"
agree "$s" "the standby after the vacuum"
agree "$p" "the primary"
before=$(answers "$p" on)
crash "$p"
node_start "$p" || bail "primary restart failed"
check "the primary after a crash answers what it did" test "$(answers "$p" on)" == "$before"
node_promote "$s"
sql "$s" "SET sift.test_create_format1 = off" "INSERT INTO pairs_text SELECT m, 'tenant-9' FROM generate_series(13, 20) m" "INSERT INTO events SELECT 20000 + i, 1 + i % 60, timestamptz '2027-01-01' + i * interval '1 second', 1 FROM generate_series(1, 50) i" "VACUUM events" >/dev/null || bail "the promoted standby does not write"
agree "$s" "the promoted standby after its own writes"
finish
