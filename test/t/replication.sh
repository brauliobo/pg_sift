#!/usr/bin/env bash
# Replication (README "Replication"). Physical: a hot standby from pg_basebackup answers the primary's queries (matches, phrases,
# count(*), BM25 scores, filter pushdown), also after inserts, updates, deletes, VACUUM, CREATE INDEX CONCURRENTLY and REINDEX,
# and takes writes once promoted. Logical: only table data replicates; the subscriber installs pg_sift and builds its own index
# (before or after the initial sync) and the apply worker maintains it. Nodes are processes (scripts/pgtest.sh tap replication)
# or containers (scripts/replication.sh --container).
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_REPL_ROWS 20000 5000)
# statement templates, one per line, @T@ is the table. The tables differ only in the replica identity: a primary key, or FULL without one
DDL="CREATE TABLE docs (id int PRIMARY KEY, body text, published date)
CREATE TABLE docs2 (id int, body text, published date)
ALTER TABLE docs2 REPLICA IDENTITY FULL
CREATE INDEX docs_published ON docs (published)
CREATE INDEX docs2_published ON docs2 (published)"
LOAD="INSERT INTO @T@ SELECT i, 'friendly support case ' || i % 7 || ' battery ' || CASE WHEN i % 3 = 0 THEN 'squeaky' ELSE 'sturdy' END || ' w' || i, date '2026-01-01' + i % 300 FROM generate_series(1, $ROWS) i"
# change FIRST_ID UPDATE_MOD DELETE_MOD: insert new rows from FIRST_ID, rewrite and delete others
change() {
	echo "INSERT INTO @T@ SELECT i, 'friendly extra case squeaky w' || i, '2026-06-01' FROM generate_series($1, $(($1 + ROWS / 2))) i
UPDATE @T@ SET body = 'updated sturdy case w5 ' || id WHERE id % $2 = 0
DELETE FROM @T@ WHERE id % $3 = 0"
}

# sqls NODE TEMPLATE [TABLE...]: every line of the template once per table
sqls() {
	local node=$1 template=$2 t line
	local -a cmds=()
	shift 2
	for t in "${@:-x}"; do
		while IFS= read -r line; do cmds+=("${line//@T@/$t}"); done <<<"$template"
	done
	sql "$node" "${cmds[@]}"
}
ddl() { sqls "$1" "$DDL"; }

# five lines: matches, phrase, count(*), the score of every match, a pushed-down filter
digest() {
	sql "$1" "SET enable_seqscan = off" "SET sift.filter_pushdown = force" \
		"SELECT count(*), md5(string_agg(id::text, ',' ORDER BY id)) FROM $2 WHERE body @@> '(case OR w5) AND friendly NOT squeaky'" \
		"SELECT count(*) FROM $2 WHERE body @@> '\"friendly support\"'" \
		"SELECT count(*) FROM $2 WHERE body @@> 'friendly'" \
		"SELECT md5(string_agg(id || ':' || round(s::numeric, 6), ',' ORDER BY id)) FROM
			(SELECT id, sift.score(ctid) s FROM $2 WHERE body @@> 'case OR squeaky' ORDER BY sift.score(ctid) DESC LIMIT 1000000) t" \
		"SELECT count(*), md5(string_agg(id::text, ',' ORDER BY id)) FROM $2 WHERE body @@> 'friendly' AND published >= '2026-03-01'" ||
		bail "digest on $1 failed"
}

# same DESCRIPTION NODE NODE TABLE [SED]: both nodes return the same digest (SED=4d leaves the scores out)
same() {
	local a b
	a=$(digest "$2" "$4" | sed "${5-}") b=$(digest "$3" "$4" | sed "${5-}")
	if [[ $(wc -l <<<"$a") -ge 4 && $a == "$b" ]]; then ok "$1"; else not_ok "$1"; diag "$2: $a"$'\n'"$3: $b"; fi
}
quiet() { "$@" >/dev/null 2>&1; }
fails() { ! quiet "$@"; }
in_recovery() { sql "$1" "SELECT pg_is_in_recovery()"; }
sift_indexes() { sql "$1" "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam
	WHERE a.amname = 'sift' AND i.indrelid = '$2'::regclass"; }
# waits for the subscription "sub" on the subscriber: all tables synchronized (ready), and the changes made so far applied
sub_ready() { for _ in $(seq 600); do [[ $(sql subscriber "SELECT count(*) FROM pg_subscription_rel WHERE srsubstate <> 'r'") == 0 ]] && return 0; sleep 0.1; done; return 1; }
sub_catchup() {
	local lsn
	lsn=$(sql primary "SELECT pg_current_wal_lsn()")
	for _ in $(seq 600); do
		[[ $(sql primary "SELECT coalesce(bool_and(replay_lsn >= '$lsn'), false) FROM pg_stat_replication WHERE application_name = 'sub'") == t ]] && return 0
		sleep 0.1
	done
	return 1
}

# ---- physical: primary, hot standby
node_init primary 0 "$WAL_CONF
wal_level = logical
max_wal_senders = 10
max_replication_slots = 10
wal_keep_size = 256MB"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" >/dev/null || bail "create extension failed"
ddl primary >/dev/null || bail "schema failed"
sqls primary "$LOAD" docs docs2 >/dev/null || bail "load failed"
sql primary "CREATE INDEX docs_body ON docs USING sift (body)" "CREATE INDEX docs2_body ON docs2 USING sift (body)" >/dev/null ||
	bail "create index failed"

node_clone primary standby 1 "hot_standby_feedback = off"
node_start standby || bail "standby start failed"
catchup primary standby || bail "standby did not catch up"
check "standby is in recovery and replayed CREATE EXTENSION" test "$(in_recovery standby)$(sql standby "SELECT extname FROM pg_extension WHERE extname = 'pg_sift'")" == tpg_sift
same "standby answers like the primary (matches, phrase, count, scores, filter pushdown)" primary standby docs
check "standby plans SiftScan and SiftCount" test "$(sql standby "SET enable_seqscan = off" \
	"EXPLAIN (costs off) SELECT id FROM docs WHERE body @@> 'case' ORDER BY sift.score(ctid) DESC LIMIT 5" \
	"EXPLAIN (costs off) SELECT count(*) FROM docs WHERE body @@> 'case'" | grep -c -E 'SiftScan|SiftCount')" == 2
check "the standby is read-only" fails sql standby "INSERT INTO docs VALUES (-1, 'x', null)"

sqls primary "$(change $((ROWS + 1)) 13 11)" docs >/dev/null && catchup primary standby
same "standby after insert, update and delete" primary standby docs
sql primary "VACUUM docs" >/dev/null && catchup primary standby
same "standby after VACUUM" primary standby docs
sql primary "CREATE INDEX CONCURRENTLY docs_lower ON docs USING sift (lower(body))" >/dev/null && catchup primary standby
check "standby after CREATE INDEX CONCURRENTLY (valid index, same count)" test "$(sql standby "SELECT indisvalid FROM pg_index WHERE indexrelid = 'docs_lower'::regclass" \
	"SELECT count(*) FROM docs WHERE lower(body) @@> 'case squeaky'")" == "$(sql primary "SELECT 't'" "SELECT count(*) FROM docs WHERE lower(body) @@> 'case squeaky'")"
sql primary "REINDEX INDEX docs_body" >/dev/null && catchup primary standby
same "standby after REINDEX" primary standby docs
sql primary "REINDEX INDEX CONCURRENTLY docs_body" >/dev/null && catchup primary standby
same "standby after REINDEX CONCURRENTLY" primary standby docs
check "sift.verify passes on the standby" quiet sql standby "SELECT sift.verify('docs_body', true)" "SELECT sift.verify('docs_lower', true)"

# ---- logical: publisher is the primary, the subscriber is a separate cluster
sql primary "CREATE PUBLICATION pub FOR TABLE docs, docs2" >/dev/null || bail "publication failed"
node_init subscriber 2
node_start subscriber || bail "subscriber start failed"
check "CREATE EXTENSION is not replicated" test "$(sql subscriber "SELECT count(*) FROM pg_extension WHERE extname = 'pg_sift'")" == 0
sql subscriber "CREATE EXTENSION pg_sift" >/dev/null && ddl subscriber >/dev/null || bail "subscriber schema failed"
# docs gets its index before the initial sync, docs2 after it
sql subscriber "CREATE INDEX docs_body ON docs USING sift (body)" "CREATE SUBSCRIPTION sub CONNECTION '$(conninfo primary)' PUBLICATION pub" >/dev/null ||
	bail "create subscription failed"
sub_ready || bail "initial sync did not finish"
same "subscriber after the initial sync into a table with a sift index" primary subscriber docs
check "the index is not replicated" test "$(sift_indexes subscriber docs2)" == 0
sql subscriber "CREATE INDEX docs2_body ON docs2 USING sift (body)" >/dev/null || bail "subscriber index failed"
same "subscriber index built after the sync (no primary key, replica identity full)" primary subscriber docs2

sqls primary "$(change $((ROWS * 2)) 17 19)" docs docs2 >/dev/null && sub_catchup || bail "subscriber did not catch up"
same "subscriber applies insert, update and delete (primary key)" primary subscriber docs
same "subscriber applies insert, update and delete (replica identity full)" primary subscriber docs2
check "a sift index cannot be the replica identity" fails sql primary "ALTER TABLE docs REPLICA IDENTITY USING INDEX docs_body"
check "sift.verify passes on the subscriber" quiet sql subscriber "SELECT sift.verify('docs_body', true)" "SELECT sift.verify('docs2_body', true)"
# BM25 counts postings that are not yet vacuumed (DESIGN.md (a)): an index rebuilt on one side only scores differently
sql subscriber "VACUUM docs" "REINDEX INDEX docs2_body" >/dev/null
same "subscriber after its own VACUUM: same scores" primary subscriber docs
same "subscriber after its own REINDEX: same matches" primary subscriber docs2 4d
sql primary "REINDEX INDEX docs2_body" >/dev/null
same "same scores again once both sides rebuilt the index" primary subscriber docs2
sql subscriber "DROP SUBSCRIPTION sub" >/dev/null

# ---- failover: the promoted standby takes writes
node_stop primary
node_promote standby
check "promoted standby is writable" test "$(in_recovery standby)" == f
sql standby "INSERT INTO docs VALUES (900001, 'friendly promoted brandnew', null)" "VACUUM docs" >/dev/null
check "promoted standby indexes and ranks new rows" test "$(sql standby "SELECT id FROM docs WHERE body @@> 'brandnew friendly' ORDER BY sift.score(ctid) DESC LIMIT 1")" == 900001
check "sift.verify passes after promotion" quiet sql standby "SELECT sift.verify('docs_body', true)"

plan "$NTEST"
finish
