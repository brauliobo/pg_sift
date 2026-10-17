#!/bin/sh
# Builds the extension and runs every test. Needs a C compiler, make and python3 whose sqlite3 module can load
# extensions and has FTS5; no root, nothing is installed.
#
#   ./test.sh                   smoke test, baseline instruction check, unit tests + FTS5 differential on a synthetic corpus, the same with the scalar kernels forced
#                               (runtime switch) and with the vector-free build (make SIMD=none) (about 40 s)
#   SIFT_SANITIZE=1 ./test.sh   also the unit tests under ASan+UBSan and the thread stress test under TSan and ASan (about 3 min)
#   SIFT_ARTICLES=articles.db ./test.sh
#                               also the FTS5 differential on a real corpus (a copy is used; table articles(id, body))
#   SIFT_ARTICLES=articles.db SIFT_PG_EXPECTED=expected.tsv ./test.sh
#                               also the comparison with pg_sift's evaluator (bench/compare_pg.py documents the file)
set -e
cd "$(dirname "$0")"
nice make -j4
nice make -j4 isa-check
python3 -I tests/smoke.py build/sift "$(cat VERSION)"
python3 -I tests/test_sift.py
python3 -I tests/test_features.py
python3 -I tests/diff_fts5.py
SIFT_SIMD=scalar python3 -I tests/test_sift.py
SIFT_SIMD=scalar python3 -I tests/test_features.py
SIFT_SIMD=scalar python3 -I tests/diff_fts5.py
nice make -j4 SIMD=none B=build-scalar
SIFT_EXT="$PWD/build-scalar/sift" python3 -I tests/test_sift.py
SIFT_EXT="$PWD/build-scalar/sift" python3 -I tests/test_features.py
if [ -n "$SIFT_SANITIZE" ]; then
	nice make -j4 asan
	export ASAN_OPTIONS=detect_leaks=0
	for t in test_sift test_features; do
		SIFT_EXT="$PWD/build-asan/sift" LD_PRELOAD="$(cc -print-file-name=libasan.so) $(cc -print-file-name=libubsan.so)" python3 -I tests/$t.py
	done
	# shellcheck disable=SC2086  # the options are split on purpose
	for a in "-m multi -r 3 -p 3" "-c -r 3 -p 4"; do build-asan/stress -s 8 $a; done
	nice make -j4 tsan-run
fi
if [ -n "$SIFT_ARTICLES" ]; then
	work=$(mktemp -d)
	trap 'rm -rf "$work"' EXIT
	cp "$SIFT_ARTICLES" "$work/diff.db"
	python3 -I tests/diff_fts5.py --articles "$work/diff.db" --ascii --n 200
	if [ -n "$SIFT_PG_EXPECTED" ]; then
		python3 -I bench/build.py --engine sift --base "$SIFT_ARTICLES" --out "$work/pg.db" --opts stemmer=none
		python3 -I bench/compare_pg.py "$work/pg.db" "$SIFT_PG_EXPECTED"
	else
		echo "skipped: comparison with pg_sift (SIFT_PG_EXPECTED is not set)"
	fi
else
	echo "skipped: real corpus differential and comparison with pg_sift (SIFT_ARTICLES is not set; they need the 218k-article database of bench/)"
fi
[ -n "$SIFT_SANITIZE" ] || echo "skipped: sanitizer builds and thread stress test (SIFT_SANITIZE is not set)"
echo "all tests passed"
