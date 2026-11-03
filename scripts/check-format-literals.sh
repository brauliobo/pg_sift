#!/usr/bin/env bash
# A test that writes a column format minor as a number goes stale when a feature raises the minor (old_lib_refusal expected 1.34 of a tree that writes 1.37). Two rules:
#  - every place of test/t, test/sql and scripts that compares a segment format with a literal of 17 or more (`format = 41`, `column format 38` in a check) is listed in
#    scripts/format-literals.tsv (FILE, literal, why it is exact); a new one fails until it is listed, and the better fix is the constant (scripts/old-lib-check.sh: colfmt NAME);
#  - the list is pinned to the constants of src/col/api/format.h: a minor added or changed fails with the places to re-verify, until the pin of the list is updated.
#   scripts/check-format-literals.sh            check
#   scripts/check-format-literals.sh --update   rewrite the pin (after the places were verified)
#   scripts/check-format-literals.sh --selftest
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
LIST=$REPO/scripts/format-literals.tsv
# a literal compared for equality, not a range (>=, <) and not in a comment line
PATTERN='(^|[^<>!=])(format|min\(format\)|max\(format\)) *= *(1[7-9]|[2-9][0-9])\b|column format (1[7-9]|[2-9][0-9])\b'
sites() { # prints "FILE<TAB>literal" of every place
	(cd "$REPO" && git ls-files 'test/t/*.sh' 'test/t/*.pl' 'test/sql/*.sql' 'scripts/*.sh' ':!scripts/check-format-literals.sh' |
		xargs grep -nE "$PATTERN" 2>/dev/null | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(#|--)' |
		grep -oE '^[^:]+|(format|min\(format\)|max\(format\)) *= *[0-9]{2}|column format [0-9]{2}' |
		awk '/^(test|scripts)\// { f = $0; next } { n = $0; gsub(/[^0-9]/, "", n); if (n + 0 >= 17) print f "\t" n }' | sort -u)
}
pin() { sed -n 's/^#define \(SIFT_COL_FORMAT_[A-Z_0-9]*\)[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1 \2/p' "$REPO/src/col/api/format.h" | "$REPO/scripts/sha256.sh" - 2>/dev/null | cut -d' ' -f1; }
if [[ ${1:-} == --selftest ]]; then
	t=$(printf '%s\n' "x format = 41 y" "SELECT bool_and(format = 34)" "min(format) >= 35" "# column format 38" "column format 12" "format < 21" "check column format 42 ok")
	got=$(grep -E "$PATTERN" <<<"$t" | grep -vE '^[[:space:]]*(#|--)' | sort)
	want=$(printf '%s\n' "x format = 41 y" "SELECT bool_and(format = 34)" "check column format 42 ok" | sort)
	[[ $got == "$want" ]] || { echo "FAIL: the pattern matches [$got]"; exit 1; }
	echo "format-literals selftest ok"; exit 0
fi
cur=$(sites)
if [[ ${1:-} == --update ]]; then
	{ echo "# FILE, the literal, why: places that name a column format by number. Checked by scripts/check-format-literals.sh against src/col/api/format.h (pin below)."
	  echo "# pin	$(pin)"
	  while IFS=$'\t' read -r f n; do printf '%s\t%s\t%s\n' "$f" "$n" "$(grep -F "$f	$n	" "$LIST" 2>/dev/null | cut -f3)"; done <<<"$cur"
	} >"$LIST.new" && mv "$LIST.new" "$LIST"; exit 0
fi
bad=0
listed=$(grep -v '^#' "$LIST" | cut -f1,2 | sort -u)
new=$(comm -13 <(echo "$listed") <(echo "$cur"))
[[ -z $new ]] || { echo "FAIL: a column format minor written as a number, not listed in scripts/format-literals.tsv (use the constant of src/col/api/format.h, as old-lib-check.sh does with colfmt):"; echo "$new" | sed 's/^/  /'; bad=1; }
want=$(sed -n 's/^# pin\t//p' "$LIST")
if [[ $want != "$(pin)" ]]; then
	echo "FAIL: the constants of src/col/api/format.h changed since scripts/format-literals.tsv was pinned; check that none of these places still names a minor the tree no longer writes, then run scripts/check-format-literals.sh --update:"
	echo "$listed" | sed 's/^/  /'
	bad=1
fi
(( bad )) || echo "format-literals: $(wc -l <<<"$listed") listed places, constants as pinned"
exit "$bad"
