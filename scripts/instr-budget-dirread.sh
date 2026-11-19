#!/usr/bin/env bash
# What a reader pays for the directory when it has many entries, in callgrind instructions of a single-user backend: a point lookup of an index of N segments (250, 500, 1000), the instructions of
# the whole statement and those inside sift_dir_read / sift_dir_read_segs (the copy of the metapage, block 1, the root and the leaves of a chain under their share locks), and the pages the read
# copies (the blocks of the chain). The two smallest sizes have a directory in block 1; a line through them is what the read of a directory of N entries would cost without a chain, and
# what the chain adds to that is held to MAX_PCT percent of the statement at every N: past it a read validated by the version of the directory (pages of a chain are immutable until the
# horizon passes) replaces the share locks.
#
#   scripts/instr-budget-dirread.sh          (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh; needs valgrind and an installed build: scripts/pgtest.sh install)
#   SETUP_SET='SET sift.test_dir_flat_max = 4; SET sift.test_dir_leaf_max = 9;' SIZES=120 scripts/instr-budget-dirread.sh
#                                            the same directory of 120 entries as a chain of pages: what a page costs a reader (the writers keep a directory to SIFT_DIR_POLICY_MAX, so a
#                                            chain of 250 or more entries is made by the test settings)

set -euo pipefail

. "$(dirname "$0")/instr-budget-lib.sh"
MAX_PCT=${MAX_PCT:-2}
SIZES=${SIZES:-"50 120 250 500 1000"}
SETUP_SET=${SETUP_SET:-}
PAGES_FILE=${SIFT_TMP:-/tmp/sift-instr}/dirread-pages.txt

setup_sql() {
	echo "LOAD 'pg_sift'; CREATE EXTENSION pg_sift_test; CREATE TABLE pages (idx text, n int); SET sift.async_maintenance = off; SET sift.col_auto_merge = off; $SETUP_SET"
	for n in $SIZES; do
		sed "s/@N@/$n/g" <<'T'
CREATE TABLE cw@N@ (id int PRIMARY KEY, k int, v int);
CREATE INDEX cw@N@_idx ON cw@N@ USING sift_col (k, v);
DO $$ BEGIN FOR b IN 1..@N@ LOOP INSERT INTO cw@N@ VALUES (b, b % 7, b % 13); PERFORM sift.col_promote('cw@N@_idx'); END LOOP; END $$;
CREATE TABLE bw@N@ (id int PRIMARY KEY, body text);
CREATE INDEX bw@N@_idx ON bw@N@ USING sift (body) WITH (write_buffer_limit = 64, merge_fanin = 32);
DO $$ BEGIN FOR b IN 1..@N@ LOOP INSERT INTO bw@N@ VALUES (b, 'w' || (b % 50) || ' common'); PERFORM sift.promote('bw@N@_idx'); END LOOP; END $$;
INSERT INTO pages SELECT 'cw@N@_idx', chain_blocks FROM sift._test_dir_info('cw@N@_idx');
INSERT INTO pages SELECT 'bw@N@_idx', chain_blocks FROM sift._test_dir_info('bw@N@_idx');
T
	done
	echo "COPY pages TO '$PAGES_FILE';"
}

mkdir -p "$(dirname "$PAGES_FILE")"
rm -f "$PAGES_FILE"
setup_sql | { ib_begin instr-budget-dirread 56232; worst=0
for n in $SIZES; do
	for kind in col bm25; do
		if [[ $kind == col ]]; then
			q="SELECT id FROM cw$n WHERE k = 3 AND v = 5;" idx="cw${n}_idx"
		else
			q="SELECT id FROM bw$n WHERE body @@> 'w7 common' LIMIT 1;" idx="bw${n}_idx"
		fi
		{ echo "SET max_parallel_workers_per_gather = 0;"; echo "SET enable_seqscan = off;"; echo "$q"; echo "$q"; } > "$SIFT_TMP/dr.sql"
		total=$(ib_ir "$SIFT_TMP/dr.sql" PortalRun)
		read_ir=$(ib_ir "$SIFT_TMP/dr.sql" sift_dir_read sift_dir_read_segs)
		pages=$(awk -v i="$idx" '$1 == i { print $2 }' "$PAGES_FILE")
		pct=$(awk -v r="${read_ir:-0}" -v t="${total:-1}" 'BEGIN { printf "%.3f", 100.0 * r / t }')
		echo "instr-budget-dirread: $kind N=$n statement=$total directory_read=$read_ir (${pct}% of the statement) chain_pages=$pages"
		echo "$kind $n $total $read_ir" >> "$SIFT_TMP/dr.tsv"
	done
done
awk -v max="$MAX_PCT" '
	{ if (!($1 in n1)) { n1[$1] = $2; r1[$1] = $4 } else if (!($1 in n2)) { n2[$1] = $2; r2[$1] = $4 }
	  else { rows[++k] = $0 } }
	END {
		for (i = 1; i <= k; i++) {
			split(rows[i], f, " ")
			slope = (r2[f[1]] - r1[f[1]]) / (n2[f[1]] - n1[f[1]])
			flat = r1[f[1]] + slope * (f[2] - n1[f[1]])
			pct = 100.0 * (f[4] - flat) / f[3]
			printf "instr-budget-dirread: %s N=%d the chain adds %d instructions to a flat read of %d: %.3f%% of the statement\n", f[1], f[2], f[4] - flat, flat, pct
			if (pct > worst) worst = pct
		}
		exit !(worst <= max)
	}' "$SIFT_TMP/dr.tsv" || { echo "instr-budget-dirread: the chain adds more than $MAX_PCT% to a point lookup"; ib_end; exit 1; }
ib_end; }
