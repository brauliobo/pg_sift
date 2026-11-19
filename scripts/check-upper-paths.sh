#!/usr/bin/env bash
# The top-N is priced at the ordered stage, after create_ordered_paths gave the paths of its input relation to the ordered relation: add_path frees a path it rejects there (a sorted
# Gather Merge that a sort above a Gather beats), and the list of the input relation still holds it. A read of that list (a pricing that asks whether a b-tree path gives the order)
# reads freed memory, which looks like a node of another type when the chunk is reused and crashes the planner. The files that price a top-N take the fact from the relation
# (its indexlist, rows) or from the paths of the ordered relation; cost/normal.c reads the paths of the group and distinct stages, whose upper paths wrap them, and cost/cte.c the
# rows of a derived relation while it is built.
#
#   scripts/check-upper-paths.sh             src/agg/order/ and src/agg/cost/
#   scripts/check-upper-paths.sh --selftest
((BASH_VERSINFO[0] >= 4)) || { echo "needs bash 4+ (brew install bash)" >&2; exit 1; }
set -euo pipefail

PATHLIST='(->|[.])(partial_)?pathlist\b'

violations() { # FILE...: the lines that name a path list, not in comments
	local f
	for f in "$@"; do
		grep -nE "$PATHLIST" "$f" | grep -vE '^[0-9]+:[[:space:]]*(/?\*|//)' | sed "s#^#$f:#" || true
	done
}

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
	printf '/* c->rel->pathlist is not read here */\nforeach_ptr(Path, p, c->rel->pathlist)\nn = list_length(rel->partial_pathlist);\nx = rel->indexlist;\n' >"$d/model.c"
	got=$(violations "$d/model.c")
	[[ $got == "$d/model.c:2:foreach_ptr(Path, p, c->rel->pathlist)"$'\n'"$d/model.c:3:n = list_length(rel->partial_pathlist);" ]] || { echo "check-upper-paths: selftest: unexpected verdict: $got"; exit 1; }
	echo "check-upper-paths: selftest ok"
	exit 0
fi

cd "$(git rev-parse --show-toplevel)"
mapfile -t files < <(git ls-files 'src/agg/order/*.[ch]' 'src/agg/cost/*.[ch]' | grep -vE '^src/agg/cost/(normal|cte)\.c$')
mapfile -t hits < <(violations "${files[@]}")
(( ${#hits[@]} == 0 )) || {
	printf '%s\n' "${hits[@]}"
	echo "check-upper-paths: ${#hits[@]} read(s) of a path list where a top-N is priced: PostgreSQL may have freed the paths of the relation at the ordered stage; take the fact from the relation (indexlist, rows) or the paths of the ordered relation, do not guard the read"
	exit 1
}
echo "check-upper-paths: ok, no path list is read in ${#files[@]} files of the top-N and the cost model"
