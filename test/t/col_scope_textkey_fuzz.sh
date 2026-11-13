#!/usr/bin/env bash
# The scope index with a text, varchar or uuid key against the plain join under churn (test/fuzz/col_scope_textkey_fuzz.py): fact INSERTs, DELETEs, UPDATEs, pair INSERTs, DELETEs, key moves, UPDATEs of the
# key, keys the value store has never seen, a key that leaves every pair, aborted transactions and savepoints, reads inside a transaction, promotion, merge, VACUUM and REINDEX in a random order, with a long
# snapshot reader; after every step a battery (IN of one key and of a list, EXISTS, a join for each pair, a group by the key, a table of keys, a time window, a top-N) must be the rows of the plain plan, and
# every read is either stored or says a documented fallback reason. Quick tier: seeds 1-3 of 5 steps on 4,000 rows; full: 24 seeds of 10 steps on 12,000.
# SIFT_FUZZ_TEXTKEY_SEEDS / SIFT_FUZZ_ROWS / SIFT_FUZZ_STEPS override.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SEEDS=$(knob SIFT_FUZZ_TEXTKEY_SEEDS 1-24 1-3)
ROWS=$(knob SIFT_FUZZ_ROWS 12000 4000)
STEPS=$(knob SIFT_FUZZ_STEPS 10 5)

node_init tk 5 "max_wal_size = 512MB
shared_buffers = 128MB
max_locks_per_transaction = 256"
node_start tk || bail "start failed"
sql tk "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions failed"
plan 1

out=$(PGHOST=$(dir tk)/sock PGPORT=$(port tk) PGUSER=postgres SIFT_FUZZ_OUT=$(dir tk)/fuzz \
	python3 -I "$REPO/test/fuzz/col_scope_textkey_fuzz.py" --seeds "$SEEDS" --rows "$ROWS" --steps "$STEPS" 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the scope reads of a text, varchar and uuid key are the plain join in every seed ($SEEDS)" test "$rc" == 0
finish
