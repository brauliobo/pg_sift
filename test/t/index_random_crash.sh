#!/usr/bin/env bash
# Seeded random kill -9 of the whole cluster (postmaster and every child at once) while a random operation runs:
# bulk insert, update, delete, promote, merge, VACUUM, VACUUM FULL, REINDEX, REINDEX CONCURRENTLY, CREATE INDEX
# (also CONCURRENTLY). After each crash and recovery every valid sift index passes sift.verify with heapallindexed
# and answers like a sequential scan of the heap (the oracle); invalid leftovers of a killed concurrent build are
# dropped. SIFT_SEED repeats a run, SIFT_ROUNDS sets the length (default 14).
. "$(dirname "$0")/lib.bash"

ROUNDS=${SIFT_ROUNDS:-14}
SEED=${SIFT_SEED:-$((RANDOM * 32768 + RANDOM))}
RANDOM=$SEED
diag "seed $SEED, $ROUNDS rounds"
plan $((ROUNDS + 1))

node_init rk 5
node_start rk || bail "start failed"
sql rk >/dev/null <<'SQL' || bail "setup failed"
CREATE EXTENSION sift;
CREATE TABLE rk (id int, body text) WITH (autovacuum_enabled = off);
INSERT INTO rk SELECT g, concat_ws(' ', 'w' || g % 7, 'w' || g % 13, 'w' || g % 101, CASE WHEN g % 5 = 0 THEN 'rare' END, 'doc' || g)
	FROM generate_series(1, 150000) g;
CREATE INDEX rk_idx ON rk USING sift (body) WITH (mutable_limit = 64);
CREATE FUNCTION ids(q text) RETURNS text LANGUAGE plpgsql SET enable_seqscan = off AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM rk WHERE body @@> $1' INTO r USING q;
	RETURN r;
END $$;
CREATE FUNCTION seq_ids(q text) RETURNS text LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM rk WHERE body @@> $1' INTO r USING q;
	RETURN r;
END $$;
SQL

OPS=(
	"INSERT INTO rk SELECT g, 'w6 rare torn ' || g FROM generate_series(1000001 + %N%, 1100000 + %N%) g"
	"UPDATE rk SET body = 'w12 upd ' || id WHERE id % 3 = %K%"
	"DELETE FROM rk WHERE id % 11 = %D%"
	"INSERT INTO rk SELECT g, 'w1 late ' || g FROM generate_series(3000001 + %N%, 3030000 + %N%) g|SELECT sift.promote('rk_idx')"
	"SELECT sift.promote('rk_idx')"
	"SELECT sift.merge('rk_idx')"
	"VACUUM rk"
	"VACUUM FULL rk"
	"REINDEX INDEX rk_idx"
	"REINDEX INDEX CONCURRENTLY rk_idx"
	"DROP INDEX IF EXISTS rk_idx2|CREATE INDEX rk_idx2 ON rk USING sift (body)"
	"DROP INDEX IF EXISTS rk_idx2|CREATE INDEX CONCURRENTLY rk_idx2 ON rk USING sift (body)"
)

# healthy: every valid sift index verifies and the planner's index answers equal the heap's
healthy() {
	local idx term
	for idx in $(sql rk "SELECT c.relname FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid JOIN pg_am a ON a.oid = c.relam
		WHERE a.amname = 'sift' AND i.indisvalid ORDER BY 1"); do
		sql rk "SELECT sift.verify('$idx', true)" >/dev/null 2>"$(dir rk)/verify.err" || { diag "$idx: $(cat "$(dir rk)/verify.err")"; return 1; }
	done
	for term in w1 w6 w12 w100 rare late torn upd; do
		[[ $(sql rk "SELECT ids('$term') = seq_ids('$term')") == t ]] || { diag "index and heap disagree on $term"; return 1; }
	done
}

drop_invalid() {
	local idx
	for idx in $(sql rk "SELECT indexrelid::regclass FROM pg_index WHERE NOT indisvalid"); do sql rk "DROP INDEX $idx" >/dev/null; done
}

landed=0
for round in $(seq "$ROUNDS"); do
	op=${OPS[RANDOM % ${#OPS[@]}]}
	op=${op//%N%/$((RANDOM % 100000 * 10))}
	op=${op//%K%/$((RANDOM % 3))}
	op=${op//%D%/$((RANDOM % 11))}
	delay=$((RANDOM % 20 * (RANDOM % 20)))
	IFS='|' read -ra stmts <<<"$op"
	sql rk "${stmts[@]}" >/dev/null 2>&1 &
	job=$!
	sleep "0.$(printf %03d $delay)"
	kill -0 $job 2>/dev/null && landed=$((landed + 1))
	crash rk
	wait $job 2>/dev/null
	node_start rk || bail "recovery failed after: $op"
	drop_invalid
	if healthy; then ok "round $round: recovered after kill -9 ${delay}ms into: ${op:0:70}"; else not_ok "round $round: after kill -9 ${delay}ms into: $op"; fi
	# keep the corpus growing and the segments varied
	sql rk "INSERT INTO rk SELECT g, 'w1 w6 round$round ' || g FROM generate_series(9000000 + $round * 1000, 9000999 + $round * 1000) g" >/dev/null
done
diag "$landed of $ROUNDS kills landed while the operation was running"
sql rk "VACUUM rk" "SELECT sift.merge('rk_idx')" >/dev/null
check "the indexes still verify and answer after a final VACUUM and merge" healthy
finish
