#!/usr/bin/env bash
# The scope index with a text, varchar or uuid key or member against the plain join under churn (test/fuzz/col_scope_textkey_fuzz.py): fact INSERTs, DELETEs, UPDATEs, pair INSERTs, DELETEs, key moves, UPDATEs of the
# key, keys the value store has never seen, a key that leaves every pair, aborted transactions and savepoints, reads inside a transaction, promotion, merge, VACUUM and REINDEX in a random order, with a long
# snapshot reader; after every step a battery (IN of one key and of a list, EXISTS, a join for each pair, a group by the key, a table of keys, a time window, a top-N) must be the rows of the plain plan, and
# every read is either stored or says a documented fallback reason. A seed picks a key (text, varchar, uuid, integer) and a member (integer, text, varchar facts with text pairs, uuid); a second run cuts the
# journal word of a string member to 3 bits (sift.test_member_hash_bits), so that members collide; a third and a fourth run do both for a scope of two paths (--two-paths: the direct pairs and the members of
# the groups a key is granted, churned with grants of groups and memberships). Quick tier: seeds 5-8 (string members) of 5 steps on 4,000 rows, the collisions on 9-11, the two paths on 1-4; full: 32 seeds
# of 10 steps on 12,000, the collisions on 1-16, the two paths on 1-32 and 1-16 with collisions. SIFT_FUZZ_TEXTKEY_SEEDS / SIFT_FUZZ_COLLIDE_SEEDS / SIFT_FUZZ_TWOPATH_SEEDS / SIFT_FUZZ_ROWS / SIFT_FUZZ_STEPS override.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SEEDS=$(knob SIFT_FUZZ_TEXTKEY_SEEDS 1-32 5-8)
COLLIDE=$(knob SIFT_FUZZ_COLLIDE_SEEDS 1-16 9-11)
TWOPATH=$(knob SIFT_FUZZ_TWOPATH_SEEDS 1-32 1-4)
ROWS=$(knob SIFT_FUZZ_ROWS 12000 4000)
STEPS=$(knob SIFT_FUZZ_STEPS 10 5)

node_init tk 5 "max_wal_size = 512MB
shared_buffers = 128MB
max_locks_per_transaction = 256"
node_start tk || bail "start failed"
sql tk "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions failed"
plan 4

out=$(PGHOST=$(dir tk)/sock PGPORT=$(port tk) PGUSER=postgres SIFT_FUZZ_OUT=$(dir tk)/fuzz \
	python3 -I "$REPO/test/fuzz/col_scope_textkey_fuzz.py" --seeds "$SEEDS" --rows "$ROWS" --steps "$STEPS" 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the scope reads of a text, varchar and uuid key and member are the plain join in every seed ($SEEDS)" test "$rc" == 0

out=$(PGHOST=$(dir tk)/sock PGPORT=$(port tk) PGUSER=postgres SIFT_FUZZ_OUT=$(dir tk)/fuzz \
	python3 -I "$REPO/test/fuzz/col_scope_textkey_fuzz.py" --seeds "$COLLIDE" --rows "$ROWS" --steps "$STEPS" --hash-bits 3 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the same when the journal words of string values collide on 3 bits ($COLLIDE)" test "$rc" == 0

out=$(PGHOST=$(dir tk)/sock PGPORT=$(port tk) PGUSER=postgres SIFT_FUZZ_OUT=$(dir tk)/fuzz \
	python3 -I "$REPO/test/fuzz/col_scope_textkey_fuzz.py" --seeds "$TWOPATH" --rows "$ROWS" --steps "$STEPS" --two-paths 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the reads of a scope of two paths with a text, varchar or uuid key and member are the plain join in every seed ($TWOPATH)" test "$rc" == 0

out=$(PGHOST=$(dir tk)/sock PGPORT=$(port tk) PGUSER=postgres SIFT_FUZZ_OUT=$(dir tk)/fuzz \
	python3 -I "$REPO/test/fuzz/col_scope_textkey_fuzz.py" --seeds "$COLLIDE" --rows "$ROWS" --steps "$STEPS" --two-paths --hash-bits 3 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the same for two paths when the journal words collide on 3 bits ($COLLIDE)" test "$rc" == 0
finish
