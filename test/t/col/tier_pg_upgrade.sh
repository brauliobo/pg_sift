#!/usr/bin/env bash
# pg_upgrade of a sift_col index with parts in object storage: the files of the index name the policy of its parts, so sift.tier_policies must come through with them. The upgrade runs
# within this major, from a copy of its binaries on each side (lib/upgrade.bash), and the index answers as the heap does afterwards. Tiering is of Format 2.0: the index is one on both runs.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

REPO=${SIFT_REPO:?}
. "${SIFT_REPO:?}/test/t/lib/upgrade.bash"

OV=$TAPDIR/overlay
TIER=$TAPDIR/tier
trap 'pg_ctl -D "$(dir old)/data" -m immediate stop >/dev/null 2>&1; cleanup; ((NFAIL)) || rm -rf "$(dir old)" "$OV" "$TIER"' EXIT
rm -rf "$OV" "$TIER"
mkdir -p "$OV/new" "$TIER"
overlay "$(command -v pg_config)" "$OV/new" "$ROOT"
BIN=$OV/new$(pg_config --bindir)
PATH=$BIN:$PATH
plan 4

initdb_node old 2 "$BIN"
initdb_node new 3 "$BIN"
NODES+=(new)
CONF="$("$REPO/scripts/pgtest.sh" conf | grep -E '^(pg_)?sift\.')${SIFT_TESTING:+
sift.test_create_format1 = off}"
node_conf old "$CONF"
node_conf new "$CONF"
"$BIN/pg_ctl" -D "$(dir old)/data" -l "$(dir old)/postgres.log" -w -t 60 start >/dev/null 2>&1 || { start_failed old; bail "the old cluster does not start"; }
EXT=pg_sift
[[ -z ${SIFT_TESTING:-} ]] || EXT="pg_sift_test CASCADE"
qs old postgres <<SQL >/dev/null || bail "setup failed"
CREATE EXTENSION $EXT;
CREATE TABLE ev (id int8, ts timestamp NOT NULL, v int, pad text);
INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 100, md5(i::text) FROM generate_series(1, 5000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, ts, v, pad);
VACUUM ev;
INSERT INTO ev SELECT 5000 + i, localtimestamp - i * interval '1 minute', i % 100, md5((5000 + i)::text) FROM generate_series(1, 5000) i;
SELECT sift.set_tier_policy('ev_col', 'file://$TIER', '30 days');
VACUUM ev;
SQL
answer() { q "$1" postgres "SET enable_seqscan = off" "SET enable_indexscan = off" "SELECT md5(string_agg(id || ':' || ts || ':' || v || pad, ',' ORDER BY id)) FROM ev WHERE id > 0"; }
before=$(answer old)
check "the old cluster has parts of the index in the store" test "$(q old postgres "SELECT count(*) > 0 FROM sift.tier_status")" == t
"$BIN/pg_ctl" -D "$(dir old)/data" -m fast -w stop >/dev/null 2>&1 || bail "the old cluster does not stop"

(cd "$(dir new)" && "$BIN/pg_upgrade" -b "$BIN" -B "$BIN" -d "$(dir old)/data" -D "$(dir new)/data" -U postgres -p "$(port old)" -P "$(port new)" -s "$(dir new)/sock" \
	>"$(dir new)/pg_upgrade.log" 2>&1) || { tail -n 20 "$(dir new)/pg_upgrade.log"; bail "pg_upgrade failed"; }
"$BIN/pg_ctl" -D "$(dir new)/data" -l "$(dir new)/postgres.log" -w -t 60 start >/dev/null 2>&1 || { start_failed new; bail "the new cluster does not start"; }

check "the policy of the index came through pg_upgrade" test "$(q new postgres "SELECT count(*) FROM sift.tier_policies WHERE relid = 'ev_col'::regclass")" == 1
out=$(answer new 2>&1)
check "the index reads its parts in the store and answers as before" test ${#before} -eq 32 -a "$out" == "$before"
check "sift.check finds nothing" test "$(q new postgres "SELECT count(*) FROM sift.check('ev_col')" 2>&1)" == 0
finish
