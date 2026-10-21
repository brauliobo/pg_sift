#!/usr/bin/env bash
# The portable core (src/portable.list, and src/portable-col.list of the column track) reaches its host only through src/core/host.h: every #include of a listed file is
# the C library, a compiler intrinsics header, the optional lz4/zstd headers, src/core/host.h or another file of the list. A PostgreSQL, SQLite or other
# host header there means the core is no longer portable (CONTRIBUTING.md). Needs no PostgreSQL, no SQLite and no build.
#
#   scripts/check-portable.sh
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../src" && pwd)
std='assert|ctype|errno|float|inttypes|limits|locale|math|setjmp|stdarg|stdatomic|stdbool|stddef|stdint|stdio|stdlib|string|time'
intrinsics='immintrin|emmintrin|x86intrin|cpuid'
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
echo "check-portable: ok, ${#files[@]} files include only core/host.h, the C library and each other"
