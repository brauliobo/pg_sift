#!/usr/bin/env bash
# The update script of the release in progress is built from its parts: sql/<version>/NN-<context>.sql, concatenated in LC_ALL=C order into
# sql/pg_sift--<from>--<version>.sql (a build product, ignored by git). <from> is the newest minor with a script before <version>.
#
#   scripts/sql-parts.sh targets   print the generated scripts
#   scripts/sql-parts.sh build     write the generated scripts that are older than a part
#   scripts/sql-parts.sh check     every part is NN-<context>.sql with a unique number
set -euo pipefail
cd "$(dirname "$0")/.."
export LC_ALL=C

# the generated script of a parts directory: from the newest X.Y that a script of sql/ goes into, below the directory's version
target() {
	local v=${1#sql/} from
	from=$(ls sql | sed -n 's/^pg_sift--\([0-9]*[.][0-9]*\)[.]sql$/\1/p; s/^pg_sift--.*--\([0-9]*[.][0-9]*\)[.]sql$/\1/p' |
		sort -uV | awk -v v="$v" 'BEGIN { split(v, a, "."); } { split($0, b, "."); if (b[1] < a[1] || (b[1] == a[1] && b[2] < a[2])) f = $0 } END { print f }')
	echo "sql/pg_sift--$from--$v.sql"
}

scripts_targets() { for d in $(dirs); do target "$d"; done; }
dirs() { ls -d sql/[0-9]*/ 2>/dev/null | sed 's#/$##'; }

case ${1:-} in
targets) scripts_targets ;;
build)
	for d in $(dirs); do
		t=$(target "$d")
		[[ -f $t && -z $(find "$d" -name '*.sql' -newer "$t") ]] || { cat "$d"/*.sql >"$t.new" && mv "$t.new" "$t"; }
	done ;;
check)
	for d in $(dirs); do
		bad=$(ls "$d" | grep -vE '^[0-9]{2}-[a-z0-9-]+[.]sql$' || true)
		[[ -z $bad ]] || { echo "sql-parts: $d: not NN-<context>.sql: $bad"; exit 1; }
		dup=$(ls "$d" | cut -c1-2 | sort | uniq -d | head -n 1)
		[[ -z $dup ]] || { echo "sql-parts: $d: the number $dup is used by two parts"; exit 1; }
	done
	for t in $(scripts_targets); do
		git ls-files --error-unmatch "$t" >/dev/null 2>&1 || git check-ignore -q "$t" || { echo "sql-parts: $t is not in .gitignore"; exit 1; }
	done
	echo "sql-parts: parts are numbered and unique" ;;
*) sed -n '2,7p' "$0"; exit 2 ;;
esac
