#!/usr/bin/env bash
# A build whose lz4 or zstd header probe fails must stop with the error that names the switch, never drop the codec silently
# (a server built so refuses the chunks another server wrote with it). Needs a PostgreSQL pg_config; make only parses the Makefile here.
#   scripts/check-codec-probe.sh    each codec: header hidden -> error naming SIFT_NO_<CODEC>=1; hidden with SIFT_NO_<CODEC>=1 -> parses; present -> parses
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
cd "$(dirname "$0")/.."
d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
real=$(command -v "${CC:-cc}")
cat >"$d/cc" <<EOF
#!/bin/sh
case " \$* " in *" -include \$SIFT_HIDE_HEADER.h "*) exit 1 ;; esac
exec $real "\$@"
EOF
chmod +x "$d/cc"
parse() { SIFT_HIDE_HEADER=$1 make -n CC="$d/cc" "${@:2}" >/dev/null 2>"$d/err"; }
for lib in lz4 zstd; do
	up=${lib^^}
	parse "$lib" && { echo "FAIL: $lib.h hidden and the Makefile still parses"; exit 1; }
	grep -qF "pg_sift: $lib not found (install lib$lib headers, or build with SIFT_NO_$up=1)" "$d/err" || { echo "FAIL: no codec error for $lib:"; cat "$d/err"; exit 1; }
	parse "$lib" "SIFT_NO_$up=1" || { echo "FAIL: SIFT_NO_$up=1 with $lib.h hidden does not parse:"; cat "$d/err"; exit 1; }
	parse none || { echo "FAIL: the Makefile does not parse with $lib present:"; cat "$d/err"; exit 1; }
done
echo "codec probe ok"
