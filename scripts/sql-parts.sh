#!/usr/bin/env bash
# The update script of the release in progress is built from its parts: sql/<version>/NN-<context>.sql, concatenated in LC_ALL=C order into
# sql/pg_sift--<from>--<version>.sql (a build product, ignored by git). <from> is the newest minor with a script before <version>.
# Two more build products come from the same parts. The install script of the version, sql/pg_sift--<version>.sql: the base script of the shortest chain
# of scripts to <from>, then the update scripts of the chain and the one above, so that a fresh install runs one script. And, when a part is named
# NN-upgrade-check.sql, the preflight for an update that is not made yet, sql/pg_sift_upgrade_check.sql (scripts/upgrade_check_head.sql, the part,
# scripts/upgrade_check_tail.sql).
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

# the install script of a parts directory
base() { echo "sql/pg_sift--${1#sql/}.sql"; }

# the scripts that PostgreSQL runs to reach the version an update script goes from, by the shortest chain: a base install script, then one update script after the other
chain() {
	python3 -I -c '
import os, re, sys
goal = re.fullmatch(r"sql/pg_sift--([\d.]+)--[\d.]+\.sql", sys.argv[1]).group(1)
key = lambda v: tuple(int(x) for x in v.split("."))
names = sorted(os.listdir("sql"))
bases = sorted((m.group(1) for f in names for m in [re.fullmatch(r"pg_sift--([\d.]+)\.sql", f)] if m), key=key, reverse=True)
edges = {}
for f in names:
    m = re.fullmatch(r"pg_sift--([\d.]+)--([\d.]+)\.sql", f)
    if m:
        edges.setdefault(m.group(1), []).append(m.group(2))
parent = {b: None for b in bases}
queue = list(bases)
for v in queue:
    for w in edges.get(v, []):
        if w not in parent:
            parent[w] = v
            queue.append(w)
if goal not in parent:
    sys.exit("sql-parts: no chain of scripts reaches " + goal)
path = []
while goal is not None:
    path.append(goal)
    goal = parent[goal]
path.reverse()
print("sql/pg_sift--%s.sql" % path[0])
for a, b in zip(path, path[1:]):
    print("sql/pg_sift--%s--%s.sql" % (a, b))
' "$1"
}

upgrade_check_part() { compgen -G "$1/*-upgrade-check.sql" || true; }

scripts_targets() {
	for d in $(dirs); do
		target "$d"
		base "$d"
		[[ -z $(upgrade_check_part "$d") ]] || echo sql/pg_sift_upgrade_check.sql
	done
}
dirs() { ls -d sql/[0-9]*/ 2>/dev/null | sed 's#/$##'; }

# the install script of the version: one line to refuse psql, then every script of the chain without its own
build_base() {
	local t=$1 b=$2 f
	{
		echo '\echo Use "CREATE EXTENSION pg_sift" to load this file. \quit'
		for f in $(chain "$t") "$t"; do grep -v '^\\echo Use .* \\quit$' "$f"; done
	} >"$b.new.$$" && mv "$b.new.$$" "$b"
}

case ${1:-} in
targets) scripts_targets ;;
build)
	for d in $(dirs); do
		t=$(target "$d") b=$(base "$d") u=$(upgrade_check_part "$d")
		[[ -f $t && -z $(find "$d" -name '*.sql' -newer "$t") ]] || { cat "$d"/*.sql >"$t.new.$$" && mv "$t.new.$$" "$t"; }
		[[ -f $b && ! $t -nt $b ]] || build_base "$t" "$b"
		[[ -z $u || (-f sql/pg_sift_upgrade_check.sql && -z $(find "$u" scripts/upgrade_check_head.sql scripts/upgrade_check_tail.sql -newer sql/pg_sift_upgrade_check.sql)) ]] ||
			{ cat scripts/upgrade_check_head.sql "$u" scripts/upgrade_check_tail.sql >sql/pg_sift_upgrade_check.sql.new.$$ && mv sql/pg_sift_upgrade_check.sql.new.$$ sql/pg_sift_upgrade_check.sql; }
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
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
