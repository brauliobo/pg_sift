#!/usr/bin/env bash
# The fast-test budget of test/README.md ("Test budget"): a new test is small at birth, and the tests that are not yet are listed with the number of sites, which only goes down.
#   unit   a test/unit/**/*.c (not a *_bench.c: a BENCHES target that no test run executes) with a ROWS constant above 4000 or a fuzz/rounds constant above 500, and no __SANITIZE_THREAD__ / __SANITIZE_ADDRESS__ guard in the file to scale it down
#   sql    a generate_series of test/sql/*.sql of more than 50000 rows (upper bound minus a literal start) that has no comment (on its line or the line before) saying why the test needs that many rows
#   slow   the number of names in SLOW_REGRESS (scripts/ci/weights.sh), the regress tests that are jobs of their own
# scripts/test-budget.allow holds the sites that exist today: KIND TAB NAME TAB COUNT TAB REASON. A count that grew, a site that is not listed and a row that no longer matches (the
# site was fixed: delete the row or lower the count) all fail, so the list can only shrink. Fix a site by scaling it down (a smaller table with sift.test_* knobs, a sanitizer branch, a
# comment that says what the size proves), not by listing it.
#   scripts/check-test-budget.sh           the tree
#   scripts/check-test-budget.sh --list    the rows that describe the tree as it is (for a deliberate update of the allow file)
#   scripts/check-test-budget.sh --selftest
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

UNIT_ROWS=4000 UNIT_FUZZ=500 SQL_SERIES=50000

# sites DIR: "unit|sql<TAB>file<TAB>count" for every file with sites over the budget, and "slow<TAB>-<TAB>names"
sites() {
	local dir=$1 units=() sqls=()
	mapfile -t units < <(find "$dir/test/unit" -name '*.c' ! -name '*_bench.c' 2>/dev/null | LC_ALL=C sort)
	mapfile -t sqls < <(find "$dir/test/sql" -name '*.sql' 2>/dev/null | LC_ALL=C sort)
	(( ${#units[@]} == 0 )) || awk -v rows="$UNIT_ROWS" -v fuzz="$UNIT_FUZZ" '
		function flush() { if (n > 0 && !guarded) { f = file; sub(/^.*test\/unit\//, "", f); print "unit\t" f "\t" n } }
		function number(def) { if (match(def, /1[ \t]*<<[ \t]*[0-9]+$/)) { sub(/^.*<<[ \t]*/, "", def); return 2 ^ def } match(def, /[0-9]+$/); return substr(def, RSTART) + 0 }
		FNR == 1 { if (file != "") flush(); file = FILENAME; guarded = 0; n = 0 }
		/^[ \t]*(\/\/|\/?\*)/ { next }
		/__SANITIZE_(THREAD|ADDRESS)__/ { guarded = 1 }
		{
			line = $0
			while (match(line, /(#[ \t]*define[ \t]+[A-Za-z_][A-Za-z_0-9]*[ \t]+|[A-Za-z_][A-Za-z_0-9]*[ \t]*=[ \t]*)(1[ \t]*<<[ \t]*)?[0-9]+/)) {
				def = substr(line, RSTART, RLENGTH); line = substr(line, RSTART + RLENGTH)
				id = def; sub(/^#[ \t]*define[ \t]+/, "", id); sub(/[ \t=].*$/, "", id); low = tolower(id)
				if (((id ~ /^[A-Z0-9_]*ROWS[A-Z0-9_]*$/ || low ~ /^n?_?rows$/) && number(def) > rows) || (low ~ /(fuzz|round)/ && number(def) > fuzz)) n++
			}
		}
		END { if (file != "") flush() }
	' "${units[@]}"
	(( ${#sqls[@]} == 0 )) || awk -v limit="$SQL_SERIES" '
		FNR == 1 { if (file != "") flush(); file = FILENAME; n = 0; prev = "" }
		function flush() { sub(/^.*test\/sql\//, "", file); if (n > 0) print "sql\t" file "\t" n }
		{
			line = tolower($0); rest = line; big = 0
			while (match(rest, /generate_series\([^,()]*,[ \t]*[0-9][0-9_]*(e[0-9]+)?/)) {
				arg = substr(rest, RSTART, RLENGTH); rest = substr(rest, RSTART + RLENGTH)
				lo = arg; sub(/^generate_series\(/, "", lo); sub(/[ \t]*,.*$/, "", lo); sub(/^.*,[ \t]*/, "", arg); gsub(/_/, "", arg)
				if (arg + 0 - (lo ~ /^[0-9]+$/ ? lo : 1) + 1 > limit) big++
			}
			if (big && line !~ /--[ \t]*[^ \t-]/ && prev !~ /^[ \t]*--[ \t]*[^ \t-]/) n += big
			prev = line
		}
		END { if (file != "") flush() }
	' "${sqls[@]}"
	printf 'slow\t-\t%s\n' "$(sed -n '/^SLOW_REGRESS="/,/"/p' "$dir/scripts/ci/weights.sh" | tr '\n' ' ' | sed 's/^SLOW_REGRESS="//; s/".*//' | wc -w | tr -d ' ')"
}

# verify DIR ALLOW: the problems, one a line
verify() {
	awk -F'\t' '
		NR == FNR { if ($0 !~ /^#/ && NF) { have[$1 "\t" $2] = $3; if ($4 == "") print "scripts/test-budget.allow: " $1 " " $2 " has no reason" } ; next }
		{ k = $1 "\t" $2; seen[k] = 1
		  if (!(k in have)) print msg($1, $2, $3, 0)
		  else if ($3 + 0 > have[k] + 0) print msg($1, $2, $3, have[k])
		  else if ($3 + 0 < have[k] + 0) print "scripts/test-budget.allow: " $1 " " $2 " has " $3 " site(s) now, the row says " have[k] ": lower the number" }
		END { for (k in have) if (!(k in seen)) { split(k, p, "\t"); print "scripts/test-budget.allow: " p[1] " " p[2] " has no site over the budget any more: delete the row" } }
		function msg(kind, name, n, allowed,   what) {
			what = kind == "unit" ? "ROWS above 4000 or fuzz/rounds above 500 with no __SANITIZE_THREAD__ / __SANITIZE_ADDRESS__ branch that scales it down: scale the test down (test/README.md, Test budget)" \
			     : kind == "sql" ? "generate_series above 50000 rows with no comment that says why the test needs them: use a smaller table with the sift.test_* knobs, or say in a comment what the size proves" \
			     : "SLOW_REGRESS grew: a regress test that is a job of its own is a test to make small, not to list"
			return (kind == "slow" ? "scripts/ci/weights.sh: " : "test/" kind "/" name ": ") n " site(s), " allowed + 0 " allowed: " what }
	' "$2" <(sites "$1" | LC_ALL=C sort)
}

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d "${TMPDIR:-/tmp}/check-test-budget.XXXXXX") && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/test/unit" "$d/test/sql" "$d/scripts/ci"
	printf 'SLOW_REGRESS="a b c\n\td e"   # the jobs of their own\n' >"$d/scripts/ci/weights.sh"
	: >"$d/allow"
	expect() { # WHAT FILE-TEXT...: the tree has the site (a line of the verdict mentions WHAT) or none
		local got; got=$(verify "$d" "$d/allow")
		[[ $1 == none ]] && { [[ -z $got ]] || { echo "selftest: a clean tree is refused: $got"; exit 1; }; return 0; }
		grep -q -- "$1" <<<"$got" || { echo "selftest: $1 is not refused: $got"; exit 1; }
	}
	printf 'SLOW_REGRESS="a b c\n\td e"\n' >"$d/scripts/ci/weights.sh"; printf 'slow\t-\t5\tx\n' >"$d/allow"
	expect none
	printf 'enum { ROWS = 4000 };\nint fuzz_rounds = 500;\nenum { ROWS_X = 1 << 11 };\n' >"$d/test/unit/ok.c"
	printf 'int rows = 1 << 17;\n' >"$d/test/unit/shift.c"
	printf 'gr->granule_rows = 8192;\n' >"$d/test/unit/field.c"
	printf 'enum { ROWS_PRED = 100000 };\n' >"$d/test/unit/rows.c"
	printf 'c.fuzz_rounds = 4000;\n' >"$d/test/unit/fuzz.c"
	mkdir -p "$d/test/unit/sub" "$d/test/sql/sub"
	printf 'enum { ROWS = 9000 };\n' >"$d/test/unit/sub/deep.c"
	printf 'enum { ROWS = 9000 };\n' >"$d/test/unit/sub/deep_bench.c"
	printf '#define MANY_ROWS 9000\n#ifdef __SANITIZE_THREAD__\n#define SMALL 1\n#endif\n' >"$d/test/unit/guarded.c"
	printf '// ROWS = 99999\n/* fuzz_rounds = 99999 */\n' >"$d/test/unit/comment.c"
	expect 'test/unit/rows.c'; expect 'test/unit/fuzz.c'; expect 'test/unit/shift.c'; expect 'test/unit/sub/deep.c'
	got=$(verify "$d" "$d/allow"); ! grep -q 'deep_bench' <<<"$got" || { echo "selftest: a *_bench.c is refused: $got"; exit 1; }
	got=$(verify "$d" "$d/allow"); ! grep -qE 'unit/(ok|guarded|comment|field).c' <<<"$got" || { echo "selftest: a small, guarded or commented unit test is refused: $got"; exit 1; }
	rm "$d"/test/unit/{rows,fuzz,shift}.c "$d"/test/unit/sub/*.c
	printf 'select 1 from generate_series(1, 50000);\nselect 1 from generate_series(1, 49999);\n-- why: the buffer spills at 60000\nselect 1 from generate_series(1, 60000);\nselect 1 from generate_series(1, 70000); -- the sort spills\n' >"$d/test/sql/ok.sql"
	printf 'select 1 from generate_series(1, 60000);\nselect 1 from generate_series(1, 1e6);\nselect 1 from generate_series(1, 2000000);\n' >"$d/test/sql/big.sql"
	printf 'select 1 from generate_series(1, :rows);\nselect 1 from generate_series(1, 10) g, generate_series(1, 10) h;\nselect 1 from generate_series(300001, 300300);\n' >"$d/test/sql/vars.sql"
	expect 'test/sql/big.sql: 3 site'
	printf 'select 1 from generate_series(1, 70000);\n' >"$d/test/sql/sub/deep.sql"; expect 'test/sql/sub/deep.sql: 1 site'; rm "$d/test/sql/sub/deep.sql"
	got=$(verify "$d" "$d/allow"); ! grep -qE 'sql/(ok|vars).sql' <<<"$got" || { echo "selftest: a small, commented or variable series is refused: $got"; exit 1; }
	printf 'slow\t-\t5\tx\nsql\tbig.sql\t3\tx\n' >"$d/allow"; expect none
	printf 'slow\t-\t5\tx\nsql\tbig.sql\t2\tx\n' >"$d/allow"; expect 'test/sql/big.sql: 3 site(s), 2 allowed'
	printf 'slow\t-\t5\tx\nsql\tbig.sql\t4\tx\n' >"$d/allow"; expect 'lower the number'
	printf 'slow\t-\t5\tx\nsql\tbig.sql\t3\tx\nsql\tgone.sql\t1\tx\n' >"$d/allow"; expect 'gone.sql has no site over the budget any more'
	printf 'slow\t-\t5\tx\nsql\tbig.sql\t3\t\n' >"$d/allow"; expect 'big.sql has no reason'
	printf 'slow\t-\t4\tx\nsql\tbig.sql\t3\tx\n' >"$d/allow"; expect 'SLOW_REGRESS grew'
	printf 'SLOW_REGRESS="a b c\n\td e f"\n' >"$d/scripts/ci/weights.sh"; printf 'slow\t-\t5\tx\nsql\tbig.sql\t3\tx\n' >"$d/allow"; expect 'SLOW_REGRESS grew'
	echo "check-test-budget selftest ok"
	exit 0
fi

cd "$(dirname "$0")/.."
if [[ ${1:-} == --list ]]; then
	sites . | LC_ALL=C sort | awk -F'\t' '{ print $1 "\t" $2 "\t" $3 "\tpredates the budget of test/README.md" }'
	exit 0
fi
out=$(verify . scripts/test-budget.allow)
[[ -z $out ]] || { echo "$out"; exit 1; }
echo "check-test-budget: ok, $(grep -vc '^#' scripts/test-budget.allow) listed sites, none new"
