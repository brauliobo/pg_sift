#!/bin/sh
# code-id.sh DIR [OPTIONS]: the identity of the library a source tree builds, 15 hex digits: a hash of every source file of src/ and of the OPTIONS that change what is compiled
# (the -DSIFT_* flags). The shared tables of a server (col/dsmreg.c) are told apart by it: a library that is not the same code never shares a table with another, so a package
# upgrade without a restart, or a build that moves a field of a shared struct without changing its size, cannot read the bytes of the other. The same sources give the same identity
# in a checkout, a `git archive` or a tarball (no date, no path, no version); the headers the build writes (build_id.h, code_id.h) are not read.
#   code-id.sh --selftest    a field moved inside a shared struct, a changed option or a changed comment changes the identity; a copy of the tree does not
if [ "$1" = --selftest ]; then
	root=$(cd "$(dirname "$0")/.." && pwd) t=$(mktemp -d) || exit 1
	trap 'rm -rf "$t"' EXIT
	mkdir "$t/a" "$t/b" && cp -R "$root/src" "$t/a/src" && cp -R "$root/src" "$t/b/src" || exit 1
	base=$("$0" "$t/a" '-DSIFT_X') || exit 1
	[ "$base" = "$("$0" "$t/b" '-DSIFT_X')" ] || { echo "code-id: a copy of the tree has another identity"; exit 1; }
	[ "$base" != "$("$0" "$t/a" '-DSIFT_TESTING')" ] || { echo "code-id: an option that changes the code does not change the identity"; exit 1; }
	# two fields of a shared struct that have the same type and size swap places: the size of the struct does not change, its layout does
	f="$t/b/src/core/reads.c" tab=$(printf '\t')
	sed -e "/^${tab}uint64${tab}${tab}gen;/{N;s/\(.*\)\n\(.*\)/\2\n\1/;}" "$f" >"$f.new" && mv "$f.new" "$f"
	cmp -s "$t/a/src/core/reads.c" "$f" && { echo "code-id: the selftest did not change a struct"; exit 1; }
	[ "$base" != "$("$0" "$t/b" '-DSIFT_X')" ] || { echo "code-id: a field moved in a struct does not change the identity"; exit 1; }
	echo "code-id: selftest ok"
	exit 0
fi
here=$(cd "$(dirname "$0")" && pwd)
cd "$1" || exit 1
{ find src -name '*.[ch]' ! -name build_id.h ! -name code_id.h | LC_ALL=C sort | xargs cat; printf '%s\n' "$2"; } | "$here/sha256.sh" | cut -c1-15
