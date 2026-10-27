#!/usr/bin/env bash
# Instruction budget of reading the head of a stored unit (col/unit.c unit_parse), in callgrind instructions of a single-user backend: instructions do not move with the
# load of the host, so the budget is a number, not a ratio. A table whose pages hold gaps (rows deleted before the index is built) has sparse page bitmaps in every unit,
# which unit_parse validates; the aggregate below reads every unit's head. Fails when the instructions inside sift_cunit_parse_head exceed BUDGET.
#
#   scripts/instr-budget.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)
#
# Measured at 200,000 rows, 5% deleted (12 units): 908,000 instructions with a byte-wise popcount of each sparse bitmap, 435,000 with 8 bytes at a time; BUDGET is 600,000.
set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
BUDGET=${BUDGET:-600000}
ib_begin instr-budget 56230 <<'SQL'
CREATE TABLE ib (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off);
INSERT INTO ib SELECT g, g % 7, g % 1000, repeat('x', 100) FROM generate_series(1, 200000) g;
DELETE FROM ib WHERE id % 20 = 3;
VACUUM ib;
CREATE INDEX ib_idx ON ib USING sift_col (id, grp, amount);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) ib;
SQL
Q="SELECT grp, count(DISTINCT id), sum(amount) FROM ib GROUP BY grp"
{ echo "SET max_parallel_workers_per_gather = 0"; echo "SET sift.agg_preagg = off"; echo "$Q"; } > "$SIFT_TMP/ib.sql"
ir=$(ib_ir "$SIFT_TMP/ib.sql" sift_cunit_parse_head)
ib_end
echo "instr-budget: unit heads read in $ir instructions (budget $BUDGET)"
[[ -n $ir && $ir -le $BUDGET ]]
