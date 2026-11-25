# shellcheck shell=bash
# The regression tests that execute the functions a change touched: scripts/focus/NAME.tsv (made by scripts/focus-gen.sh with gcov: the tests of a function) and scripts/focus-pins.tsv (the tests
# pinned to a file). `ci.sh affected --focus` and scripts/fast-gate.sh read them here. Sourced; REPO is the checkout.
# focus_tables: the tables of scripts/focus, NUL separated, in byte order (a test in a folder has its table in the folder)
focus_tables() { find "$REPO/scripts/focus" -name '*.tsv' -print0 | LC_ALL=C sort -z; }
# focus_select TOKEN...: a token is FILE:FUNCTION; "T TEST" for each regression test that executes one or is pinned to its file, "H FILE" for each file whose function every test executes (the =hot row)
focus_select() {
	focus_tables | xargs -0 awk -F'\t' -v toks="$*" 'BEGIN { n = split(toks, a, " "); for (i = 1; i <= n; i++) want[a[i]] = 1 }
		$1 == "=hot" { n = split($2, h, " "); for (i = 1; i <= n; i++) if (h[i] in want) { split(h[i], p, ":"); print "H " p[1] }; next }
		{ n = split($2, h, " "); for (i = 1; i <= n; i++) if (h[i] in want) { print "T " $1; break } }'
	awk -F'\t' -v toks="$*" 'BEGIN { n = split(toks, a, " "); for (i = 1; i <= n; i++) { split(a[i], p, ":"); want[p[1]] = 1 } }
		/^#/ { next }
		$1 in want { n = split($2, t, " "); for (i = 1; i <= n; i++) print "T " t[i] }' "$REPO/scripts/focus-pins.tsv"
}
# focus_file_tests FILE...: the regression tests that execute any function of each file (the change is known by file, not by hunk), the pinned ones first
focus_file_tests() {
	local f pat=()
	for f; do pat+=(-e "$f:"); done
	{ awk -F'\t' -v files="$*" 'BEGIN { n = split(files, a, " "); for (i = 1; i <= n; i++) want[a[i]] = 1 }
		/^#/ { next }
		$1 in want { n = split($2, t, " "); for (i = 1; i <= n; i++) print "0 " t[i] }' "$REPO/scripts/focus-pins.tsv"
	  focus_tables | xargs -0 grep -hF "${pat[@]}" | awk -F'\t' '$1 != "=hot" { print "1 " $1 }'; } | sort -k1,1n -k2,2 | cut -d' ' -f2 | awk '!seen[$0]++'
}
# focus_tokens BASE: FILE:FUNCTION of every hunk of the diff against BASE in src/; the function is the last line at column 0 that starts like a definition before the hunk
focus_tokens() {
	local f l
	for f in $(cd "$REPO" && git diff --name-only --no-renames "$1" -- src); do
		[[ -f $REPO/$f ]] || continue
		git -C "$REPO" diff -U0 --no-renames "$1" -- "$f" | sed -n '/^@@ -0,0 /d; s/^@@ [^+]*+\([0-9]*\).*/\1/p' | while read -r l; do
			awk -v f="$f" -v l="$l" 'NR <= l && /^[A-Za-z_][A-Za-z0-9_]*\(/ { n = $0; sub(/\(.*/, "", n) } END { print f ":" (n == "" ? "*" : n) }' "$REPO/$f"
		done
	done | sort -u
}
