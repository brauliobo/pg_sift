#!/usr/bin/env bash
# Instruction budget of the TID fetches of the UPDATEs of a sift_table that has grown, in callgrind instructions of a single-user backend: instructions do not move with the load
# of the host, so the budget is a number, not a ratio. 2,000 rows 500 apart of a table of 1,000,000 rows (a unit each, more superblocks than the fetch scans) are updated by key in
# four statements over the same rows. The fetch scans kept 32 superblocks and 8 MB, so a superblock was dropped before the next statement came back to its rows and each fetch parsed
# its heads again (the 40,000 updates of tbl_update_keys cost 2.3 G instructions at 300,000 rows and 3.5 G at 1,000,000 once the write buffer was settled, the heap twin 4.2 G); they
# keep 256 superblocks and 64 MB. Fails when the instructions of the statements exceed BUDGET.
#
#   scripts/instr-budget-fetch.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)

set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
BUDGET=${BUDGET:-760000000}
ib_begin instr-budget-fetch 56231 <<'SQL'
CREATE TABLE ibf (id int PRIMARY KEY, v int, k int, note text) USING sift_table WITH (autovacuum_enabled = off);
INSERT INTO ibf SELECT g, 0, g % 1000, 'n' || g FROM generate_series(1, 1000000) g;
SELECT sift.table_maintain('ibf');
ANALYZE ibf;
SQL
{ echo "SET max_parallel_workers_per_gather = 0"
  for _ in 1 2 3 4; do echo "UPDATE ibf SET v = v + 1 WHERE id IN (SELECT g * 500 + 7 FROM generate_series(0, 1999) g)"; done; } > "$SIFT_TMP/ibf.sql"
ir=$(ib_ir "$SIFT_TMP/ibf.sql" PortalRun)
ib_end
echo "instr-budget-fetch: 4 x 2,000 updates by key in 1,000,000 rows in $ir instructions (budget $BUDGET)"
[[ -n $ir && $ir -le $BUDGET ]]
