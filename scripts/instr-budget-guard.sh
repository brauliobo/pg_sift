#!/usr/bin/env bash
# What the guard of a write (core/quarantine.c: the subtransaction that lets a write on a damaged index end as a skip, not as a failed statement) costs a DELETE of one row of an index
# with track_changes, counted in callgrind instructions of a single-user backend. The guard's subtransaction takes no transaction id of its own: the records the write stamps carry the
# id of the level that called it (sift_quarantine_write_xid), so the instructions inside GetNewTransactionId of MORE guarded statements are those of MORE statements without a guard
# (sift.on_corruption = error: the id of the statement itself), where a record stamped with the id of the subtransaction assigns a second one for each (the assignment, a lock on it and
# its release, about 2.4 k instructions of a statement of 60 k).
#
#   scripts/instr-budget-guard.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)

set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
N=${N:-3}
MORE=${MORE:-5}

{ echo "LOAD 'pg_sift'; SET sift.async_maintenance = off; SET sift.col_auto_merge = off;
CREATE TABLE g (id int PRIMARY KEY, k int, v int);
CREATE INDEX g_idx ON g USING sift_col (id, k, v) WITH (track_changes = id);
DO \$\$ BEGIN FOR b IN 1..$N LOOP INSERT INTO g SELECT x, x % 7, x % 13 FROM generate_series(b * 1000, b * 1000 + 99) x; PERFORM sift.col_promote('g_idx'); END LOOP; END \$\$;"; } | { ib_begin instr-budget-guard 56235
deletes() { for i in $(seq "$2"); do echo "DELETE FROM g WHERE id = $((1000 + i));"; done; }
run() { local f=$SIFT_TMP/gd.sql; { echo "SET sift.col_auto_merge = off; SET sift.on_corruption = $1;"; cat; } > "$f"; ib_ir "$f" "${@:2}"; }
xid_guard=$(deletes g "$MORE" | run quarantine GetNewTransactionId)
xid_plain=$(deletes g "$MORE" | run error GetNewTransactionId)
ib_end
echo "instr-budget-guard: GetNewTransactionId for $MORE statements: $xid_guard with the guard, $xid_plain without (bound x1.05)"
awk -v xg="$xid_guard" -v xp="$xid_plain" 'BEGIN { exit !(xp > 0 && xg <= xp * 1.05) }'
}
