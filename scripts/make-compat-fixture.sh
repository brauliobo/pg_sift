#!/usr/bin/env bash
# Regenerates test/fixtures/compat2x (SIFT_FIXTURE_OUT=test/fixtures/compat30 for a checkout of 3.0, whose directory entry
# kind is named buffer where 2.x says mutable): the relation files of a small table and of the sift index on it, written
# by the 2.x library, which test/t/compat_2x_fixture.sh puts under the build being tested to prove that it still
# reads (and maintains) indexes written by 2.x. Run it only to change the fixture; the files are committed.
#
#   SIFT_2X_REPO=/path/to/checkout-of-2.x SIFT_TMP=/tmp/sift-fixture SIFT_PORT=56280 scripts/make-compat-fixture.sh
#
# SIFT_2X_REPO is a checkout of the 2.x library; it is built (scripts/pgtest.sh install, production build) and run
# from there. SIFT_TMP (removed at the start and at the end) and SIFT_PORT follow scripts/pgtest.sh.
#
# Provenance of the committed fixture: PostgreSQL 18 (data checksums on, which PostgreSQL 17 clusters without
# checksums read all the same), pg_sift 2.0.0 = main 6655974 (the 2.0.1 branch robust/m201-count, ab3b7fb, changes no
# format), extension version 1.2. Frozen heap, so no transaction status is needed in the cluster it is put into.
#
# The table: docs (id int, body text), 3000 rows with ids 1..3000 inserted in three batches, body = 'alpha<id%7>
# beta<id%13> w<id%50>' + ' gamma' when id % 3 = 0 + ' rareterm' when id % 250 = 0 + id % 5 times ' filler'.
# The index: docs_idx USING sift (body) WITH (vacuum_segments = 8, dead_rewrite_pct = 100), the defaults otherwise
# (positions on), made so that VACUUM neither merges nor rewrites a segment. Built in this order:
#   1. ids 1..1500, then CREATE INDEX      -> immutable segment gen 1
#   2. ids 1501..2900, sift.promote        -> immutable segment gen 4
#   3. DELETE id <= 1500 AND id % 10 = 3 (150 rows), VACUUM -> gen 1 is dirty with 150 dead documents
#   4. ids 2901..3000                      -> 100 documents in the write buffer gen 3
#   5. VACUUM FREEZE, CHECKPOINT, clean shutdown, copy of the files
# sift.segments (gen kind ndocs dirty dead_docs): 1 immutable 1500 t 150 | 3 mutable 100 f | 4 immutable 1400 f 0.
# 2850 rows are left (sum(id) 4389300). expected.txt is the output of queries.sql (counts and sum(id) of eight queries,
# the top 12 ids and scores of two ranked queries, sift.segments, sift.verify) on the 2.x build; the test compares the
# build under test with it.
#
# Files: heap, heap_vm, heap_fsm (main, visibility map and free space map forks of docs), index (main fork of
# docs_idx), expected.txt, queries.sql, info.txt (PostgreSQL version, 2.x commit, the WAL position the files were
# written up to).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SIFT_2X_REPO:?set SIFT_2X_REPO to a checkout of the 2.x library}
OUT=${SIFT_FIXTURE_OUT:-$REPO/test/fixtures/compat2x}
export SIFT_TMP=${SIFT_TMP:?set SIFT_TMP} SIFT_PORT=${SIFT_PORT:?set SIFT_PORT} SIFT_TESTING=
PGTEST=$SRC/scripts/pgtest.sh
DATA=$SIFT_TMP/data

"$PGTEST" destroy
trap '"$PGTEST" destroy' EXIT
nice "$PGTEST" install >/dev/null
"$PGTEST" start
q() { "$PGTEST" psql -q -At -v ON_ERROR_STOP=1 "$@"; }

q >/dev/null <<'SQL'
ALTER SYSTEM SET autovacuum = off;
ALTER SYSTEM SET sift.async_maintenance = off;
SELECT pg_reload_conf();
CREATE EXTENSION pg_sift;
CREATE TABLE docs (id int, body text);
CREATE FUNCTION fill(lo int, hi int) RETURNS void LANGUAGE sql BEGIN ATOMIC
	INSERT INTO docs SELECT g, concat_ws(' ', 'alpha' || g % 7, 'beta' || g % 13, 'w' || g % 50,
			CASE WHEN g % 3 = 0 THEN 'gamma' END, CASE WHEN g % 250 = 0 THEN 'rareterm' END, repeat('filler ', g % 5))
		FROM generate_series(lo, hi) g;
END;
SELECT fill(1, 1500);
CREATE INDEX docs_idx ON docs USING sift (body) WITH (vacuum_segments = 8, dead_rewrite_pct = 100);
SELECT fill(1501, 2900);
SELECT sift.promote('docs_idx');
DELETE FROM docs WHERE id <= 1500 AND id % 10 = 3;
VACUUM docs;
SELECT fill(2901, 3000);
VACUUM FREEZE docs;
DROP FUNCTION fill(int, int);
CHECKPOINT;
SQL

# the layout the header promises
layout=$(q -c "SELECT string_agg(gen || ' ' || kind || ' ' || ndocs || ' ' || dirty || ' ' || coalesce(dead_docs::text, ''), ' | ' ORDER BY gen) FROM sift.segments('docs_idx')")
[[ ${layout// mutable / buffer } == "1 immutable 1500 true 150 | 3 buffer 100 false  | 4 immutable 1400 false 0" ]] || { echo "unexpected segments: $layout" >&2; exit 1; }

heap=$(q -c "SELECT pg_relation_filepath('docs')")
index=$(q -c "SELECT pg_relation_filepath('docs_idx')")
lsn=$(q -c "SELECT pg_current_wal_lsn()")
version=$(q -c "SHOW server_version")

mkdir -p "$OUT"
q -f "$REPO/test/fixtures/compat2x/queries.sql" >"$OUT/expected.txt"

"$PGTEST" stop
cp "$DATA/$heap" "$OUT/heap"
cp "$DATA/${heap}_vm" "$OUT/heap_vm"
cp "$DATA/${heap}_fsm" "$OUT/heap_fsm"
cp "$DATA/$index" "$OUT/index"
cat >"$OUT/info.txt" <<EOF
postgresql $version
pg_sift $(git -C "$SRC" rev-parse --short HEAD) ($(git -C "$SRC" describe --tags --always))
wal_lsn $lsn
segments $layout
EOF
du -sk "$OUT"
