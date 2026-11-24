#!/usr/bin/env bash
# The portable core (src/portable.list, and src/portable-col.list of the column track) reaches its host only through src/core/host.h: every #include of a listed file is
# the C library, a compiler intrinsics header, the optional lz4/zstd headers, src/core/host.h or another file of the list. A PostgreSQL, SQLite or other
# host header there means the core is no longer portable (CONTRIBUTING.md). Then every listed file is compiled (syntax only) against the SQLite host header
# (sqlite/host/host.h, which the MariaDB host includes): a PostgreSQL-only type, macro or function that core/host.h gets from the server headers and the portable
# hosts do not provide fails here, where the SQLite and MariaDB builds would fail only in the CI jobs of those ports. Then the SQLite build (sqlite/) is linked with --no-undefined (Linux), where a function that only the PostgreSQL files define fails. Needs a C compiler and make; no PostgreSQL, no SQLite library.
#
#   scripts/check-portable.sh
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../src" && pwd)
std='assert|ctype|errno|float|inttypes|limits|locale|math|setjmp|stdarg|stdatomic|stdbool|stddef|stdint|stdio|stdlib|string|time'
intrinsics='immintrin|emmintrin|x86intrin|cpuid|arm_neon|sys/auxv'
compress='lz4|lz4hc|zstd|zdict'	# the optional libraries of src/col/compress.c, behind SIFT_HAVE_LZ4 / SIFT_HAVE_ZSTD
mapfile -t files < <(cat "$SRC/portable.list" "$SRC/portable-col.list" | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$')
names=("${files[@]}" "${files[@]##*/}")	# a file is included by its path from src/
bad=0
printf '%s\n' "${files[@]}" | awk -v src="$SRC" -v ok_angle="^($std|$intrinsics|$compress)[.]h\$" -v names=" ${names[*]} " '
	{
		f = $0; path = src "/" f
		r = (getline line < path)
		if (r < 0) { print "portable.list: " f " does not exist"; bad = 1; next }
		if (r == 0) next
		n = 0
		do {
			n++
			if (line !~ /^[ \t]*#[ \t]*include/) continue
			inc = line
			if (match(line, /^[ \t]*#[ \t]*include[ \t]*[<"][^>"]+[>"]/)) { inc = substr(line, RSTART, RLENGTH); sub(/^[ \t]*#[ \t]*include[ \t]*[<"]/, "", inc); sub(/[>"]$/, "", inc) }
			if (index(line, "<")) { if (inc ~ ok_angle) continue }
			else if (inc == "core/host.h" || index(names, " " inc " ")) continue
			print f ":" n ": " line "  (a portable file includes only the C library, intrinsics, core/host.h and files of portable.list)"; bad = 1
		} while ((getline line < path) > 0)
		close(path)
	}
	END { exit bad }
' || bad=1
if ((bad)); then exit 1; fi
ROOT=$(dirname "$SRC")
if command -v "${CC:-cc}" >/dev/null; then
	cc_dir=$(mktemp -d); trap 'rm -rf "$cc_dir"' EXIT
	slots=${PORTABLE_JOBS:-4}; running=0
	for f in "${files[@]}"; do
		[[ $f == *.c ]] || continue
		(( running < slots )) || { wait -n; running=$((running - 1)); }
		{ "${CC:-cc}" -fsyntax-only -Werror=implicit-function-declaration -Wno-unused-function -D_GNU_SOURCE -DSIFT_HOST_SQLITE -I"$ROOT/sqlite/host" -I"$SRC" -I"$ROOT/sqlite" "$SRC/$f" >"$cc_dir/${f//\//_}.log" 2>&1 \
			|| { cat "$cc_dir/${f//\//_}.log"; echo "$f: does not compile against the portable host (sqlite/host/host.h): a PostgreSQL-only symbol, or one the host must provide"; } >"$cc_dir/${f//\//_}.bad"; } &
		running=$((running + 1))
	done
	wait
	for f in "${files[@]}"; do [[ ! -e $cc_dir/${f//\//_}.bad ]] || { cat "$cc_dir/${f//\//_}.bad"; bad=1; }; done
	if ((bad)); then exit 1; fi
	# the link: a shared object links with undefined symbols, so a portable file that calls into a PostgreSQL-only one (sift_wt_write_seghdr of core/writer.c, called from bm25/writer.c)
	# builds and fails only when SQLite loads it; the build of sqlite/ links with --no-undefined, the sift.so and the col-smoke executable of portable-col.list (GNU ld: the other linkers refuse an undefined symbol by default)
	if [[ $(uname -s) == Linux ]] && command -v make >/dev/null; then
		b=$(mktemp -d) && trap 'rm -rf "$b" "$cc_dir"' EXIT
		LC_ALL=C make -s -j4 -C "$ROOT/sqlite" B="$b" EXTRA=-Wl,--no-undefined all "$b/col-smoke" >"$b/make.log" 2>&1 \
			|| { grep -E "undefined|error" "$b/make.log" | head -n 10; echo "the SQLite build does not link without undefined symbols: a portable file needs a function the SQLite host does not have"; exit 1; }
	fi
fi
echo "check-portable: ok, ${#files[@]} files include only core/host.h, the C library and each other, and the SQLite build links without undefined symbols"
