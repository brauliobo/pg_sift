#!/usr/bin/env bash
# Where the bytes of a BM25 index go (docs/perf/BM25-SIZE.md): builds the index of TABLE.COLUMN with the default options and with
# positions = off, one segment each, and prints the parts of sift._test_segment_parts with their bytes, bytes per posting, bits per
# posting and bytes per term, as a Markdown table. Needs a testing build (SIFT_TESTING=1, the pgtest.sh default) and a started cluster
# (eval "$(scripts/pgtest.sh env)" or PG* variables); the database must hold the table and pg_sift. Drops the two indexes it makes.
#
#   scripts/bm25-size.sh [--table T] [--column C] [--opts "reloption = value, ..."] [--keep]
#
# --opts adds reloptions to both builds (grouped_min_df is the GUC sift.grouped_min_df: PGOPTIONS="-c sift.grouped_min_df=1000000000").
set -euo pipefail

TABLE=docs COLUMN=body OPTS= KEEP=
while (( $# )); do
	case $1 in
		--table) TABLE=$2; shift ;;
		--column) COLUMN=$2; shift ;;
		--opts) OPTS=$2; shift ;;
		--keep) KEEP=1 ;;
		*) sed -n '2,10p' "$0"; exit 1 ;;
	esac
	shift
done
Q() { psql -X -q -At -v ON_ERROR_STOP=1 "$@"; }

Q -c "CREATE EXTENSION IF NOT EXISTS pg_sift_test" >/dev/null
for variant in default nopos; do
	idx=bm25size_$variant
	opts=$OPTS
	[[ $variant == nopos ]] && opts="positions = off${OPTS:+, $OPTS}"
	Q -c "DROP INDEX IF EXISTS $idx" -c "SET max_parallel_maintenance_workers = 0" \
		-c "CREATE INDEX $idx ON $TABLE USING sift ($COLUMN)${opts:+ WITH ($opts)}" >/dev/null
	echo
	echo "### $TABLE.$COLUMN, $variant${opts:+ ($opts)}"
	echo
	Q -c "
WITH p AS (SELECT sum(ndocs) ndocs, sum(nterms) nterms FROM sift.segments('$idx')),
     n AS (SELECT sum(postings) postings FROM sift.segment_postings('$idx')),
     parts AS (SELECT part, sum(bytes) bytes FROM sift._test_segment_parts('$idx') GROUP BY part)
SELECT '| part | bytes | B/posting | bits/posting | B/term |' || E'\n' || '|---|---:|---:|---:|---:|' || E'\n' ||
       string_agg(format('| %s | %s | %s | %s | %s |', part, to_char(bytes, 'FM999,999,999,999'), round(bytes::numeric / n.postings, 3),
                         round(8 * bytes::numeric / n.postings, 2), round(bytes::numeric / p.nterms, 2)),
                  E'\n' ORDER BY (part = 'segment total'), (part = 'other (page headers, segment header, unused end)'), part)
FROM parts, p, n GROUP BY n.postings, p.nterms
UNION ALL SELECT format('%s documents, %s terms, %s postings in the blobs and inline; relation %s bytes (%s MiB)', p.ndocs, p.nterms, n.postings,
                        pg_relation_size('$idx'), round(pg_relation_size('$idx') / 1048576.0, 2)) FROM p, n"
	[[ -n $KEEP ]] || Q -c "DROP INDEX $idx"
done
