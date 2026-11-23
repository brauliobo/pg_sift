# shellcheck shell=bash
# The regression tests that execute the functions a change touched: scripts/focus/NAME.tsv (made by scripts/focus-gen.sh with gcov: the tests of a function) and scripts/focus-pins.tsv (the tests
# pinned to a file). `ci.sh affected --focus` reads them here. Sourced; REPO is the checkout.
# focus_select TOKEN...: a token is FILE:FUNCTION; "T TEST" for each regression test that executes one or is pinned to its file, "H FILE" for each file whose function every test executes (the =hot row)
focus_select() {
	awk -F'\t' -v toks="$*" 'BEGIN { n = split(toks, a, " "); for (i = 1; i <= n; i++) want[a[i]] = 1 }
		$1 == "=hot" { n = split($2, h, " "); for (i = 1; i <= n; i++) if (h[i] in want) { split(h[i], p, ":"); print "H " p[1] }; next }
		{ n = split($2, h, " "); for (i = 1; i <= n; i++) if (h[i] in want) { print "T " $1; break } }' "$REPO"/scripts/focus/*.tsv
	awk -F'\t' -v toks="$*" 'BEGIN { n = split(toks, a, " "); for (i = 1; i <= n; i++) { split(a[i], p, ":"); want[p[1]] = 1 } }
		/^#/ { next }
		$1 in want { n = split($2, t, " "); for (i = 1; i <= n; i++) print "T " t[i] }' "$REPO/scripts/focus-pins.tsv"
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
