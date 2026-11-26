#!/usr/bin/env bash
# A metadata slab claimed from a free extent of its class (core/f2/fsm/slab.c): the extent leaves its list in the record that makes it the slab, so an error between the two (the injection point
# sift-fsm-claim-slab, PostgreSQL built with --enable-injection-points: scripts/pg-assert.sh --inject) leaves it free in its list, never in neither, and the next claim takes it.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }

POINT=sift-fsm-claim-slab
plan 7
node_init cs 3 "autovacuum = off
sift.async_maintenance = off"
node_start cs || bail "start failed"
sql cs "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE EXTENSION injection_points" "CREATE TABLE s (v int)" \
	"SET sift.test_create_format1 = off" "CREATE INDEX s_idx ON s USING sift_store_test (v)" "SELECT sift._test_f2_init('s_idx')" >/dev/null || bail "setup failed"

fsm() { sql cs "SELECT sift._test_f2_fsm('s_idx')"; }
pages=0
meta_alloc() { pages=$((pages + 1)); out=$(sql cs "SELECT sift._test_f2_meta_alloc('s_idx')" 2>&1); }

# a free extent of 16 blocks, the class of the third slab; the metadata pages are taken (from the slabs or the pool of the file) until the slabs fall under their reserve and the claim runs
hole=$(sql cs "SELECT (sift._test_f2_alloc('s_idx', 16)).start")
sql cs "SELECT sift._test_f2_alloc('s_idx', 1)" "SELECT sift._test_f2_release('s_idx', $hole, 16, 'none')" "SELECT sift._test_f2_reclaim('s_idx')" >/dev/null || bail "the free extent failed"
out=$(fsm)
check "the extent waits in its class beside two slabs" grep -q "c4=1 .*/16$" <<<"$out"

sql cs "SELECT injection_points_attach('$POINT', 'error')" >/dev/null
while ((pages < 64)) && meta_alloc; do :; done
check "the claim stops at $POINT" grep -q "error triggered for injection point $POINT" <<<"$out"
sql cs "SELECT injection_points_detach('$POINT')" >/dev/null
out=$(fsm)
check "the extent is still free in its class after the error" grep -q "c4=1 .*/16$" <<<"$out"
out=$(sql cs "SELECT sift._test_f2_check('s_idx')")
check "the allocator checks clean after the error" grep -q "^parts=0 " <<<"$out"

while ((pages < 128)) && [[ $(fsm) != *c4=0* ]]; do meta_alloc || bail "a metadata page failed: $out"; done
out=$(fsm)
check "the next claim takes the extent as the third slab" grep -q "c4=0 .*/32$" <<<"$out"
out=$(sql cs "SELECT sift._test_f2_check('s_idx')")
check "the allocator checks clean after the claim" grep -q "^parts=0 " <<<"$out"
out=$(sql cs "SELECT sift._test_f2_sweep('s_idx')")
check "the sweep frees the pages the test took and the block after the extent, and nothing of the extent ($out of $((pages + 1)))" test "$out" == $((pages + 1))

finish
