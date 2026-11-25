#!/usr/bin/env bash
# A scope over a text or uuid member column of an index in format major 2 on a primary, a hot standby, a primary that crashed and a promoted standby: the summary is cut by the scope key, the member is
# named by its code in the scope key store of the member column of the pairs relation, and every node answers the scoped statements as the plain join does. The primary adds a member that no summary
# has, moves and drops pairs, inserts, updates and deletes facts, and vacuums; the standby replays the stores of the codes with the pages of the summary.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 15

p=f2m_primary
s=f2m_standby
node_init "$p" 6 "wal_level = replica
max_wal_senders = 4
autovacuum = off
sift.async_maintenance = off"
node_start "$p" || bail "primary start failed"
sql "$p" "CREATE EXTENSION pg_sift" "SET sift.test_create_format1 = off" \
	"CREATE TABLE ev_text (id int PRIMARY KEY, member text NOT NULL, created_at timestamptz NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false)" \
	"CREATE TABLE ev_uuid (id int PRIMARY KEY, member uuid NOT NULL, created_at timestamptz NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO ev_text SELECT i, 'member-' || (1 + i % 60), timestamptz '2026-01-01' + i * interval '97 minutes', i % 101 FROM generate_series(1, 4000) i" \
	"INSERT INTO ev_uuid SELECT i, md5('m' || (1 + i % 60))::uuid, timestamptz '2026-01-01' + i * interval '97 minutes', i % 101 FROM generate_series(1, 4000) i" \
	"CREATE TABLE pm_text (member text NOT NULL, key int NOT NULL)" "CREATE UNIQUE INDEX pm_text_u ON pm_text (member, key)" \
	"CREATE TABLE pm_uuid (member uuid NOT NULL, key int NOT NULL)" "CREATE UNIQUE INDEX pm_uuid_u ON pm_uuid (member, key)" \
	"INSERT INTO pm_text SELECT 'member-' || m, (m + j) % 9 FROM generate_series(1, 60) m, generate_series(0, 2) j WHERE j < m % 4" \
	"INSERT INTO pm_uuid SELECT md5('m' || m)::uuid, (m + j) % 9 FROM generate_series(1, 60) m, generate_series(0, 2) j WHERE j < m % 4" \
	"CREATE INDEX ev_text_i ON ev_text USING sift_col (member, created_at, amount) WITH (scope = 'k: member -> pm_text(member, key)', track_changes = 'ctid')" \
	"CREATE INDEX ev_uuid_i ON ev_uuid USING sift_col (member, created_at, amount) WITH (scope = 'k: member -> pm_uuid(member, key)', track_changes = 'ctid')" \
	"VACUUM ANALYZE ev_text" "VACUUM ANALYZE ev_uuid" >/dev/null || bail "setup failed"
check "both indexes are in format major 2 with a summary" test "$(sql "$p" "SELECT string_agg(format_major || ':' || (scopes > 0), ',' ORDER BY relation::text) FROM sift.status() WHERE relation::text LIKE 'ev\_%'")" == "2:true,2:true"
node_clone "$p" "$s" 7 "hot_standby_feedback = on"
node_start "$s" || bail "standby start failed"
check "the standby catches up" catchup "$p" "$s"

# the scoped statements of the two member types: the node's answer with the aggregate on, then the join's (aggregate off); one string each
statements() {
	local t k
	for t in text uuid; do
		for k in "p.key = 3" "p.key = 9" "p.key IN (0, 8, 99)" "p.key = 1 AND e.created_at >= '2026-02-01'"; do
			echo "SELECT count(*), sum(e.amount) FROM ev_$t e JOIN pm_$t p ON p.member = e.member WHERE $k"
		done
		echo "SELECT p.key, count(*), sum(e.amount) FROM ev_$t e JOIN pm_$t p ON p.member = e.member GROUP BY p.key"
		echo "SELECT e.id, e.created_at, e.amount FROM ev_$t e JOIN pm_$t p ON p.member = e.member WHERE p.key = 2 ORDER BY e.created_at DESC LIMIT 5"
	done
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
	"INSERT INTO pm_text SELECT 'member-' || m, 9 FROM generate_series(1, 12) m" "INSERT INTO pm_uuid SELECT md5('m' || m)::uuid, 9 FROM generate_series(1, 12) m" \
	"DELETE FROM pm_text WHERE key = 0" "DELETE FROM pm_uuid WHERE key = 0" "UPDATE pm_text SET key = 1 WHERE key = 4 AND member = 'member-4'" \
	"INSERT INTO ev_text SELECT 10000 + i, 'member-' || (1 + i % 70), timestamptz '2026-09-01' + i * interval '11 minutes', i % 13 FROM generate_series(1, 500) i" \
	"INSERT INTO ev_uuid SELECT 10000 + i, md5('m' || (1 + i % 70))::uuid, timestamptz '2026-09-01' + i * interval '11 minutes', i % 13 FROM generate_series(1, 500) i" \
	"DELETE FROM ev_text WHERE id % 37 = 0" "DELETE FROM ev_uuid WHERE id % 37 = 0" "UPDATE ev_text SET amount = amount + 500 WHERE id % 41 = 0" >/dev/null || bail "primary writes failed"
check "the standby catches up with the pairs and the facts" catchup "$p" "$s"
agree "$s" "the standby after the writes (no summary has the new members)"
sql "$p" "VACUUM ev_text" "VACUUM ev_uuid" >/dev/null || bail "vacuum failed"
check "the standby catches up with the vacuum" catchup "$p" "$s"
agree "$s" "the standby after the vacuum"
agree "$p" "the primary"
before=$(answers "$p" on)
crash "$p"
node_start "$p" || bail "primary restart failed"
check "the primary after a crash answers what it did" test "$(answers "$p" on)" == "$before"
node_promote "$s"
sql "$s" "SET sift.test_create_format1 = off" "INSERT INTO pm_text SELECT 'member-' || m, 9 FROM generate_series(13, 20) m" "INSERT INTO ev_text SELECT 20000 + i, 'member-' || (1 + i % 60), timestamptz '2027-01-01' + i * interval '1 second', 1 FROM generate_series(1, 50) i" "VACUUM ev_text" >/dev/null || bail "the promoted standby does not write"
agree "$s" "the promoted standby after its own writes"
finish
