#!/usr/bin/env bash
# The scope index of a sift_col index against the plain join under churn (test/fuzz/col_scope.py): fact INSERTs, DELETEs, UPDATEs (also of the entity), upserts, pair INSERTs, DELETEs, moves of an
# entity between keys (entities in 0 to 6 keys), UPDATEs of the key of a pair, a reload of the pairs, aborted transactions and savepoints, promotion, merge, VACUUM and REINDEX in a random order, with a
# long snapshot reader; after every step a battery (one key, sets of keys counted once, per pair, count/sum/distinct/top-N, the SQL forms) must be the plain join through the Format 2.0 scope node, the
# V6 scan of sift.in_scope and the plain plan, and right after a build the count of one key of a tracked index is read from the summary. Quick tier: seeds 1-2 and 19 (pairs whose btree leads with the key,
# patched by the changed members of a session that read them) of 5 steps on 6,000 rows; full: 24 seeds of 8 steps on 12,000 (about 3 minutes).
# Every third seed is a union seed (the scope of a direct and a joined path: grants of a member, grants of a group, memberships; see the docstring of the script for --twohop-strict).
# The duplicates family (--family dups) makes the pairs the distinct rows of a btree that is not unique, a pair being several rows.
# SIFT_FUZZ_SEEDS / SIFT_FUZZ_DUP_SEEDS / SIFT_FUZZ_ROWS / SIFT_FUZZ_STEPS override.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

SEEDS=$(knob SIFT_FUZZ_SEEDS 1-24 1-2,19)
ROWS=$(knob SIFT_FUZZ_ROWS 12000 6000)
STEPS=$(knob SIFT_FUZZ_STEPS 8 5)
DUP_SEEDS=$(knob SIFT_FUZZ_DUP_SEEDS 1-24 1-2)

node_init sf 5 "max_wal_size = 512MB
shared_buffers = 128MB
max_locks_per_transaction = 256"
node_start sf || bail "start failed"
sql sf "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions failed"
plan 2

out=$(PGHOST=$(dir sf)/sock PGPORT=$(port sf) PGUSER=postgres SIFT_FUZZ_OUT=$(dir sf)/fuzz \
	python3 -I "$REPO/test/fuzz/col_scope.py" --seeds "$SEEDS" --rows "$ROWS" --steps "$STEPS" 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the scope reads are the plain join in every seed ($SEEDS)" test "$rc" == 0

out=$(PGHOST=$(dir sf)/sock PGPORT=$(port sf) PGUSER=postgres SIFT_FUZZ_OUT=$(dir sf)/fuzz \
	python3 -I "$REPO/test/fuzz/col_scope.py" --family dups --seeds "$DUP_SEEDS" --rows "$ROWS" --steps "$STEPS" 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the scope reads of pairs in several rows are the plain statements in every seed ($DUP_SEEDS)" test "$rc" == 0
finish
