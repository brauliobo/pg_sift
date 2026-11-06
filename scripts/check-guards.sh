#!/usr/bin/env bash
# No two headers share an include guard (needs no PostgreSQL and no build). A translation unit that includes both would silently lose the declarations of the one
# it reaches second; the compiler says nothing. The guard of a header is the name of its first #ifndef when the next line #defines it.
#
#   scripts/check-guards.sh            every tracked header
#   scripts/check-guards.sh --selftest
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

duplicates() {
	local f
	for f; do
		awk '/^#[ \t]*ifndef[ \t]+[A-Za-z_0-9]+/ { n = $2; getline; if ($1 == "#define" && $2 == n) print n; exit }' "$f" | sed "s|\$|\t$f|"
	done | sort | awk -F'\t' '{ c[$1]++; f[$1] = f[$1] " " $2 } END { bad = 0; for (g in c) if (c[g] > 1) { print "duplicate include guard " g ":" f[g]; bad = 1 } exit bad }'
}

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d)
	trap 'rm -rf "$d"' EXIT
	for n in a:X b:Y c:X; do printf '#ifndef %s_H\n#define %s_H\n#endif\n' "${n#*:}" "${n#*:}" >"$d/${n%:*}.h"; done
	duplicates "$d/a.h" "$d/b.h" || { echo "check-guards: selftest: distinct guards reported"; exit 1; }
	! duplicates "$d/a.h" "$d/c.h" >/dev/null || { echo "check-guards: selftest: a shared guard passed"; exit 1; }
	echo "check-guards: selftest ok"
	exit 0
fi

cd "$(dirname "$0")/.."
mapfile -d '' -t headers < <(git ls-files -z '*.h')
duplicates "${headers[@]}"
echo "check-guards: no two of the ${#headers[@]} headers share an include guard"
