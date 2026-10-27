#!/usr/bin/env bash
# Instruction budget of the zone pruning of a scan whose filter is an OR of ranges (a zone tree), in callgrind instructions of a single-user backend: instructions do not
# move with the load of the host, so the budget is a number, not a ratio. The index has 3,127 units (cut by a key, 64 rows each) and the filter keeps 4 of them; the aggregate
# began a scan for every morsel of two units, each testing its units again (3,125 units pruned, about 1,565 begins, 7,054,000 instructions); the kept map tests each unit once
# (47,000). Fails when the instructions inside sift_col_begin_zones and sift_col_kept_map exceed BUDGET.
#
#   scripts/instr-budget-zone.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)
set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
BUDGET=${BUDGET:-500000}
ib_begin instr-budget-zone 56230 <<'SQL'
CREATE TABLE ibz (id int, k int, grp int, amount int) WITH (autovacuum_enabled = off);
INSERT INTO ibz SELECT g, g / 64, g % 7, g % 1000 FROM generate_series(1, 200000) g;
CREATE INDEX ibz_idx ON ibz USING sift_col (id, k, grp, amount) WITH (key_cut = 'k', key_cut_rows = 64);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) ibz;
SQL
{ echo "SET max_parallel_workers_per_gather = 0"; echo "SET sift.agg_preagg = off"; echo "SET sift.agg_force = on"
  echo "SELECT grp, sum(amount) FROM ibz WHERE id < 100 OR id BETWEEN 5000 AND 5100 GROUP BY grp"; } > "$SIFT_TMP/ibz.sql"
ir=$(ib_ir "$SIFT_TMP/ibz.sql" sift_col_begin_zones sift_col_kept_map)
ib_end
echo "instr-budget-zone: zone pruning of an OR of ranges over 3,127 units in $ir instructions (budget $BUDGET)"
[[ -n $ir && $ir -le $BUDGET ]]
