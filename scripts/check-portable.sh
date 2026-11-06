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
intrinsics='immintrin|emmintrin|x86intrin|cpuid|arm_neon'
compress='lz4|lz4hc|zstd'	# the optional libraries of src/col/compress.c, behind SIFT_HAVE_LZ4 / SIFT_HAVE_ZSTD
mapfile -t files < <(cat "$SRC/portable.list" "$SRC/portable-col.list" | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$')
names=("${files[@]}" "${files[@]##*/}")	# a file is included by its path from src/
bad=0
for f in "${files[@]}"; do
	[[ -f $SRC/$f ]] || { echo "portable.list: $f does not exist"; bad=1; continue; }
	while IFS=: read -r n line; do
		inc=$(sed -E 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]+)[>"].*/\1/' <<<"$line")
		case $line in
		*'<'*)
			[[ $inc =~ ^($std|$intrinsics|$compress)\.h$ ]] && continue ;;
		*)
			[[ $inc == core/host.h ]] && continue
			[[ " ${names[*]} " == *" $inc "* ]] && continue ;;
		esac
		echo "$f:$n: $line  (a portable file includes only the C library, intrinsics, core/host.h and files of portable.list)"
		bad=1
	done < <(grep -nE '^[[:space:]]*#[[:space:]]*include' "$SRC/$f" || true)
done
if ((bad)); then exit 1; fi
ROOT=$(dirname "$SRC")
if command -v "${CC:-cc}" >/dev/null; then
	for f in "${files[@]}"; do
		[[ $f == *.c ]] || continue
		"${CC:-cc}" -fsyntax-only -Werror=implicit-function-declaration -Wno-unused-function -D_GNU_SOURCE -DSIFT_HOST_SQLITE -I"$ROOT/sqlite/host" -I"$SRC" -I"$ROOT/sqlite" "$SRC/$f" \
			|| { echo "$f: does not compile against the portable host (sqlite/host/host.h): a PostgreSQL-only symbol, or one the host must provide"; bad=1; }
	done
	if ((bad)); then exit 1; fi
	# the link: a shared object links with undefined symbols, so a portable file that calls into a PostgreSQL-only one (sift_wt_write_seghdr of core/writer.c, called from bm25/writer.c)
	# builds and fails only when SQLite loads it; the build of sqlite/ links with --no-undefined (GNU ld: the other linkers refuse an undefined symbol by default)
	if [[ $(uname -s) == Linux ]] && command -v make >/dev/null; then
		b=$(mktemp -d) && trap 'rm -rf "$b"' EXIT
		LC_ALL=C make -s -j4 -C "$ROOT/sqlite" B="$b" EXTRA=-Wl,--no-undefined all >"$b/make.log" 2>&1 \
			|| { grep -E "undefined|error" "$b/make.log" | head -n 10; echo "the SQLite build does not link without undefined symbols: a portable file needs a function the SQLite host does not have"; exit 1; }
	fi
fi
echo "check-portable: ok, ${#files[@]} files include only core/host.h, the C library and each other, and the SQLite build links without undefined symbols"
