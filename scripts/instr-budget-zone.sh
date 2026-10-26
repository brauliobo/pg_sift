#!/usr/bin/env bash
# Instruction budget of the zone pruning of a scan whose filter is an OR of ranges (a zone tree), in callgrind instructions of a single-user backend: instructions do not
# move with the load of the host, so the budget is a number, not a ratio. The index has 3,127 units (cut by a key, 64 rows each) and the filter keeps 4 of them; the aggregate
# began a scan for every morsel of two units, each testing its units again (3,125 units pruned, about 1,565 begins, 7,054,000 instructions); the kept map tests each unit once
# (47,000). Fails when the instructions inside sift_col_begin_zones and sift_col_kept_map exceed BUDGET.
#
#   scripts/instr-budget-zone.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUDGET=${BUDGET:-500000}
command -v valgrind >/dev/null || { echo "instr-budget-zone: valgrind is not installed, skipped"; exit 0; }
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-instr} SIFT_PORT=${SIFT_PORT:-56230}
cd "$REPO"
scripts/pgtest.sh stop >/dev/null 2>&1 || true
rm -rf "$SIFT_TMP/data"
scripts/pgtest.sh start >/dev/null
eval "$(scripts/pgtest.sh env | grep '^export')"
P="psql -X -q -At -d postgres"
$P -c "CREATE EXTENSION pg_sift" -c "ALTER DATABASE postgres SET sift.col_auto_merge = off" >/dev/null
$P >/dev/null <<'SQL'
CREATE TABLE ibz (id int, k int, grp int, amount int) WITH (autovacuum_enabled = off);
INSERT INTO ibz SELECT g, g / 64, g % 7, g % 1000 FROM generate_series(1, 200000) g;
CREATE INDEX ibz_idx ON ibz USING sift_col (id, k, grp, amount) WITH (key_cut = 'k', key_cut_rows = 64);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) ibz;
SQL
scripts/pgtest.sh stop >/dev/null
{ echo "SET max_parallel_workers_per_gather = 0"; echo "SET sift.agg_preagg = off"; echo "SET sift.agg_force = on"
  echo "SELECT grp, sum(amount) FROM ibz WHERE id < 100 OR id BETWEEN 5000 AND 5100 GROUP BY grp"; } > "$SIFT_TMP/ibz.sql"
ir=$(valgrind --tool=callgrind --collect-atstart=no --toggle-collect=sift_col_begin_zones --toggle-collect=sift_col_kept_map --callgrind-out-file="$SIFT_TMP/ibz.cg" \
	postgres --single -D "$SIFT_TMP/data" -c sift.async_maintenance=off -c jit=off -c autovacuum=off postgres < "$SIFT_TMP/ibz.sql" 2>&1 >/dev/null | sed -n 's/.*Collected : *\([0-9]*\).*/\1/p' | tail -n 1)
rm -rf "$SIFT_TMP/data"
echo "instr-budget-zone: zone pruning of an OR of ranges over 3,127 units in $ir instructions (budget $BUDGET)"
[[ -n $ir && $ir -le $BUDGET ]]
