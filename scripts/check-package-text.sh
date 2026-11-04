#!/usr/bin/env bash
# The one-line descriptions of the packages stay under 80 characters: Homebrew (brew style, desc), Debian (lintian, synopsis), RPM (rpmlint, Summary) and Arch (pkgdesc) reject a longer line,
# and only a hosted job runs those tools. Brew also rejects a trailing period and a leading article.
#   scripts/check-package-text.sh [DIR]    check the tree under DIR (default: the repo)
#   scripts/check-package-text.sh --selftest   a 79-character line passes; 80 characters, a trailing period and a leading article fail
set -uo pipefail
T=$'\t'   # BSD sed (macOS) reads no \t in a replacement
lines() { # DIR: "file<TAB>description" for each packaging file
	(cd "$1" && {
		sed -n "s/^  desc \"\(.*\)\"\$/Formula\/pg_sift.rb${T}\1/p" Formula/pg_sift.rb
		sed -n "s/^pkgdesc='\(.*\)'\$/packaging\/PKGBUILD${T}\1/p" packaging/PKGBUILD
		sed -n "s/^Description: \(.*\)\$/packaging\/debian\/control${T}\1/p" packaging/debian/control
		sed -n "s/^Summary: *\(.*\)\$/packaging\/pg_sift.spec${T}\1/p" packaging/pg_sift.spec
	}) 2>/dev/null
}
verdict() { # reads lines(); prints a failure per line that is too long, ends in a period or starts with an article; at least four lines must be there
	awk -F'\t' '{ n++ } length($2) >= 80 { print $1 ": " length($2) " characters (under 80)" } $2 ~ /\.$/ { print $1 ": ends in a period" } $2 ~ /^(A|An|The) / { print $1 ": starts with an article" } END { if (n != 4) print "found " n " of 4 descriptions" }'
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/Formula" "$d/packaging/debian"
	mk() { printf '  desc "%s"\n' "$1" >"$d/Formula/pg_sift.rb"; printf "pkgdesc='%s'\n" "$1" >"$d/packaging/PKGBUILD"; printf 'Description: %s\n' "$1" >"$d/packaging/debian/control"; printf 'Summary:        %s\n' "$1" >"$d/packaging/pg_sift.spec"; }
	ok=$(printf 'x%.0s' $(seq 79)); mk "$ok"
	[[ -z $(lines "$d" | verdict) ]] || { echo "FAIL: 79 characters are rejected: $(lines "$d" | verdict)"; exit 1; }
	for bad in "${ok}x" "${ok%x}." "The fast ${ok:0:40}"; do mk "$bad"; [[ $(lines "$d" | verdict | grep -c .) == 4 ]] || { echo "FAIL: '$bad' is not rejected in all four files"; exit 1; }; done
	mk "$ok"; rm "$d/packaging/PKGBUILD"; [[ $(lines "$d" | verdict) == 'found 3 of 4'* ]] || { echo "FAIL: a missing description is not reported"; exit 1; }
	echo "package-text selftest ok"; exit 0
fi
bad=$(lines "${1:-$(cd "$(dirname "$0")/.." && pwd)}" | verdict)
[[ -z $bad ]] || { echo "FAIL: a package description the packaging tools reject:"; echo "$bad"; exit 1; }
echo "package descriptions ok"
