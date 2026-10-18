#!/usr/bin/env bash
# The portable core (src/portable.list) reaches its host only through src/sift_host.h: every #include of a listed file is
# the C library, a compiler intrinsics header, src/sift_host.h or another file of the list. A PostgreSQL, SQLite or other
# host header there means the core is no longer portable (CONTRIBUTING.md). Needs no PostgreSQL, no SQLite and no build.
#
#   scripts/check-portable.sh
set -euo pipefail

SRC=$(cd "$(dirname "$0")/../src" && pwd)
std='assert|ctype|errno|float|inttypes|limits|locale|math|setjmp|stdarg|stdatomic|stdbool|stddef|stdint|stdio|stdlib|string|time'
intrinsics='immintrin|emmintrin|x86intrin|cpuid'
mapfile -t files < <(grep -v '^[[:space:]]*#' "$SRC/portable.list" | grep -v '^[[:space:]]*$')
bad=0
for f in "${files[@]}"; do
	[[ -f $SRC/$f ]] || { echo "portable.list: $f does not exist"; bad=1; continue; }
	while IFS=: read -r n line; do
		inc=$(sed -E 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]+)[>"].*/\1/' <<<"$line")
		case $line in
		*'<'*)
			[[ $inc =~ ^($std|$intrinsics)\.h$ ]] && continue ;;
		*)
			[[ $inc == sift_host.h ]] && continue
			[[ " ${files[*]} " == *" $inc "* ]] && continue ;;
		esac
		echo "$f:$n: $line  (a portable file includes only the C library, intrinsics, sift_host.h and files of portable.list)"
		bad=1
	done < <(grep -nE '^[[:space:]]*#[[:space:]]*include' "$SRC/$f" || true)
done
if ((bad)); then exit 1; fi
echo "check-portable: ok, ${#files[@]} files include only sift_host.h, the C library and each other"
