#!/bin/sh
# Builds the extension and runs every test. Needs a C compiler, make and python3 whose sqlite3 module can load
# extensions and has FTS5; no root, nothing is installed.
#
#   ./test.sh                   unit tests + FTS5 differential on a synthetic corpus (about 10 s)
#   SIFT_ARTICLES=articles.db ./test.sh
#                               also the FTS5 differential on a real corpus (a copy is used; table articles(id, body))
#   SIFT_ARTICLES=articles.db SIFT_PG_EXPECTED=expected.tsv ./test.sh
#                               also the comparison with pg_sift's evaluator (bench/compare_pg.py documents the file)
set -e
cd "$(dirname "$0")"
nice make -j4
python3 -I tests/test_sift.py
python3 -I tests/diff_fts5.py
if [ -n "$SIFT_ARTICLES" ]; then
	work=$(mktemp -d)
	trap 'rm -rf "$work"' EXIT
	cp "$SIFT_ARTICLES" "$work/diff.db"
	python3 -I tests/diff_fts5.py --articles "$work/diff.db" --ascii --n 200
	if [ -n "$SIFT_PG_EXPECTED" ]; then
		python3 -I bench/build.py --engine sift --base "$SIFT_ARTICLES" --out "$work/pg.db" --opts stemmer=none
		python3 -I bench/compare_pg.py "$work/pg.db" "$SIFT_PG_EXPECTED"
	fi
fi
echo "all tests passed"
