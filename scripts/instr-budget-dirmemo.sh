#!/usr/bin/env bash
# What a write statement pays for a chained directory, in callgrind instructions of a single-user backend: an index of N segments whose directory is forced into a chain of pages
# (the test settings of scripts/instr-budget-dirread.sh), and a lone DELETE of one row, whose statement writes one tombstone and no segment. A backend reads the chain's pages once
# per version of the directory (core/dirchain.c keeps the entries and the pages it listed): its first statement pays that read, the others copy them. Bounds, on the instructions
# inside chain_read (the pages read and checked), against REF, those of the one read that sift._test_dir_info makes:
#   - the first DELETE reads the chain once, and MORE of them read it as often as one: a statement that reads the pages again (the tombstone log, the lookup of its dead addresses
#     and the open of the directory each did, four reads for one statement) fails;
#   - the first INSERT, which adds a segment and so writes a new version of the directory, reads the chain once as well;
#   - a statement after the first costs at most MAX_PCT percent more than the same statement on an index of FLAT_N segments, whose directory is block 1.
#
#   scripts/instr-budget-dirmemo.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)

set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
N=${N:-120}
FLAT_N=${FLAT_N:-3}
MORE=${MORE:-4}
MAX_PCT=${MAX_PCT:-10}
READ_X=${READ_X:-1.1}

table() {
	cat <<T
CREATE TABLE $1 (id int PRIMARY KEY, k int, v int);
CREATE INDEX $1_idx ON $1 USING sift_col (id, k, v) WITH (track_changes = id);
DO \$\$ BEGIN FOR b IN 1..$2 LOOP INSERT INTO $1 VALUES (b, b % 7, b % 13); PERFORM sift.col_promote('$1_idx'); END LOOP; END \$\$;
T
}

{ echo "LOAD 'pg_sift'; CREATE EXTENSION pg_sift_test; SET sift.async_maintenance = off; SET sift.col_auto_merge = off; SET sift.test_dir_flat_max = 4; SET sift.test_dir_leaf_max = 9;"
  table chained "$N"
  echo "RESET sift.test_dir_flat_max; RESET sift.test_dir_leaf_max;"
  table flat "$FLAT_N"; } | { ib_begin instr-budget-dirmemo 56234
deletes() { for i in $(seq "$2"); do echo "DELETE FROM $1 WHERE id = $i;"; done; }
inserts() { for i in $(seq "$2"); do echo "INSERT INTO $1 VALUES ($((1000 + i)), 1, 1);"; done; }
run() { local f=$SIFT_TMP/dm.sql; { echo "SET sift.col_auto_merge = off;"; cat; } > "$f"; ib_ir "$f" "$@"; }
chain_ref=$(echo "SELECT chain_blocks FROM sift._test_dir_info('chained_idx');" | run chain_read)
chain_one=$(deletes chained 1 | run chain_read)
chain_more=$(deletes chained "$MORE" | run chain_read)
chain_first_insert=$(inserts chained 1 | run chain_read)
stmt_one=$(deletes chained 1 | run PortalRun)
stmt_more=$(deletes chained "$MORE" | run PortalRun)
flat_one=$(deletes flat 1 | run PortalRun)
flat_more=$(deletes flat "$MORE" | run PortalRun)
ib_end
warm_chain=$(( (stmt_more - stmt_one) / (MORE - 1) ))
warm_flat=$(( (flat_more - flat_one) / (MORE - 1) ))
echo "instr-budget-dirmemo: $N segments: chain_read $chain_ref for one read, $chain_one for one DELETE and $chain_more for $MORE, $chain_first_insert for the first INSERT; a statement after the first $warm_chain against $warm_flat on $FLAT_N segments (bounds: x$READ_X of a read, +$MAX_PCT%)"
awk -v ref="$chain_ref" -v one="$chain_one" -v more="$chain_more" -v ins="$chain_first_insert" -v wc="$warm_chain" -v wf="$warm_flat" -v pct="$MAX_PCT" -v x="$READ_X" \
	'BEGIN { exit !(ref > 0 && one <= ref * x && more <= one * 1.02 && ins <= ref * x && wc <= wf * (1 + pct / 100)) }'
}
