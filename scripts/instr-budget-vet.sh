#!/usr/bin/env bash
# Instruction budget of the heap check of the rows of an index that no VACUUM covers (no page all-visible: a table loaded and not vacuumed, or one an old transaction
# holds back), in callgrind instructions of a single-user backend: instructions do not move with the load of the host, so the budget is a number, not a ratio. The aggregate
# below sends every row through sift_vis_filter_page; the instructions inside it (the buffer reads of the pages included) are a cost of each row. Fails over BUDGET.
#
#   scripts/instr-budget-vet.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)
#
# Measured at 200,000 rows: 20,024,000 instructions with a call through a TID test for each row, 12,030,000 with the test inline; BUDGET is 16,000,000.
set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
BUDGET=${BUDGET:-16000000}
ib_begin instr-budget-vet 56231 <<'SQL'
CREATE TABLE vb (id int, grp int, amount int) WITH (autovacuum_enabled = off);
INSERT INTO vb SELECT g, g % 50, g % 1000 FROM generate_series(1, 200000) g;
CREATE INDEX vb_idx ON vb USING sift_col (id, grp, amount);
SELECT count(*) FROM vb;
SQL
Q="SELECT grp, count(*), sum(amount) FROM vb GROUP BY grp"
{ echo "SET max_parallel_workers_per_gather = 0"; echo "$Q"; } > "$SIFT_TMP/vb.sql"
ir=$(ib_ir "$SIFT_TMP/vb.sql" sift_vis_filter_page)
ib_end
echo "instr-budget-vet: 200,000 rows checked by the heap in $ir instructions (budget $BUDGET)"
[[ -n $ir && $ir -le $BUDGET ]]
