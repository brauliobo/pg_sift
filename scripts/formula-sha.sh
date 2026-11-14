#!/usr/bin/env bash
# The stable stanza of the Homebrew formula points at the tag tarball of the release, whose hash exists only once the tag does: scripts/release.sh runs this after the tag is pushed.
#   scripts/formula-sha.sh VERSION [FORMULA] [--tarball FILE]   set url and sha256 of FORMULA (default Formula/pg_sift.rb) to the tarball of vVERSION; the archive is downloaded
#                                                               twice and the two files must be byte-identical (the hash is the one a user gets); --tarball FILE uses FILE
#   scripts/formula-sha.sh --selftest
set -euo pipefail
cd "$(dirname "$0")/.."
sha() { scripts/sha256.sh "$1" | cut -d' ' -f1; }

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	printf '  homepage "https://example.com/pg_sift"\n  url "https://example.com/pg_sift/archive/refs/tags/v1.0.0.tar.gz"\n  sha256 "%064d"\n  head "x"\n' 0 >"$d/f.rb"
	echo tarball >"$d/t.tgz"
	"$0" 5.0.1 "$d/f.rb" --tarball "$d/t.tgz" >/dev/null
	grep -qF '  url "https://example.com/pg_sift/archive/refs/tags/v5.0.1.tar.gz"' "$d/f.rb" || { echo "FAIL: the url is not that of the tag"; exit 1; }
	grep -qF "  sha256 \"$(sha "$d/t.tgz")\"" "$d/f.rb" || { echo "FAIL: the hash is not that of the tarball"; exit 1; }
	grep -qF '  head "x"' "$d/f.rb" || { echo "FAIL: the head stanza changed"; exit 1; }
	"$0" 5.0 "$d/f.rb" --tarball "$d/t.tgz" 2>/dev/null && { echo "FAIL: a version without a patch number is accepted"; exit 1; }
	echo "formula-sha selftest ok"; exit 0
fi

ver=${1:?usage: formula-sha.sh VERSION [FORMULA] [--tarball FILE]}; shift
f=Formula/pg_sift.rb tb=
while (( $# )); do case $1 in --tarball) tb=${2:?}; shift 2 ;; *) f=$1; shift ;; esac; done
[[ $ver =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version is MAJOR.MINOR.PATCH" >&2; exit 2; }
url=$(sed -n 's/^  homepage "\(.*\)"/\1/p' "$f")/archive/refs/tags/v$ver.tar.gz
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
if [[ -n $tb ]]; then cp "$tb" "$w/a.tgz"; cp "$tb" "$w/b.tgz"
else for i in a b; do curl -fsSL -o "$w/$i.tgz" "$url"; done; fi
cmp -s "$w/a.tgz" "$w/b.tgz" || { echo "two downloads of $url differ: no hash" >&2; exit 1; }
new=$(sha "$w/a.tgz")
old=$(sed -n 's/^  sha256 "\(.*\)"/\1/p' "$f")
sed -i.bak -e "s#^  url \".*\"#  url \"$url\"#" -e "s#^  sha256 \".*\"#  sha256 \"$new\"#" "$f" && rm -f "$f.bak"
echo "$f: v$ver sha256 $new (was $old)"
