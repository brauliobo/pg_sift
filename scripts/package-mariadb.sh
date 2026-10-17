#!/usr/bin/env bash
# Build and smoke-test the release tarball of the MariaDB extension (sift-mariadb-VERSION-linux-x86_64.tar.gz: libsift_udf.so for each
# supported MariaDB series, sql/functions.sql, sql/sift.sql, LICENSE, NOTICE, README.md, BUILD-INFO) for a git ref, in throwaway docker
# containers of the stock MariaDB images. The source is `git archive REF`; the recipe (packaging/mariadb/) and the smoke test
# (mariadb/tests/smoke.sh) come from this checkout. The version is the tag (v1.4.0 -> 1.4.0) and must equal mariadb/VERSION at the tag;
# a ref that is not a tag gets mariadb/VERSION-git<sha>.
#
#   scripts/package-mariadb.sh [all] [REF [OUTDIR]]   build, test every series, write OUTDIR/SHA256SUMS
#   scripts/package-mariadb.sh build REF OUTDIR       build only
#   scripts/package-mariadb.sh test TARBALL [SERIES...]  smoke-test TARBALL in a fresh container of the image of each SERIES (default: SERIES)
#
# REF defaults to HEAD, OUTDIR to dist/. The library calls the server's own symbols, so it is built against the headers of one MariaDB
# series and refuses to load into another (sift_version() and every function say so): the tarball holds one library per series of SERIES
# (space-separated, default below), each built in the stock image mariadb:<series> against the server headers of that very image
# (MariaDB's own apt repository pins them to the server's exact version) and loaded into a server of the same image by the smoke
# test. BUILD-INFO in the tarball lists the server version and the glibc each library needs.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SERIES=${SERIES:-10.11 11.4 11.8 12.1 13.0}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

version() { # version REF: the tag without "v" (which must match mariadb/VERSION), else VERSION-git<sha>
	local ref=$1 file tag
	file=$(git -C "$REPO" show "$ref:mariadb/VERSION" 2>/dev/null) || { echo "$ref has no mariadb/VERSION: it predates the MariaDB extension" >&2; return 1; }
	if tag=$(git -C "$REPO" describe --tags --exact-match "$ref" 2>/dev/null); then
		[[ ${tag#v} == "$file" ]] || { echo "tag $tag but mariadb/VERSION is $file: set mariadb/VERSION before tagging" >&2; return 1; }
		echo "$file"
	else
		echo "$file-git$(git -C "$REPO" rev-parse --short "$ref")"
	fi
}

build() { # build REF OUTDIR
	local ref=$1 out ver epoch src work name s; mkdir -p "$2" && out=$(cd "$2" && pwd)
	ver=$(version "$ref"); epoch=$(git -C "$REPO" log -1 --format=%ct "$ref"); name=sift-mariadb-$ver-linux-x86_64
	src=$(mktemp -d -p "$TMP"); work=$(mktemp -d -p "$TMP")
	git -C "$REPO" archive "$ref" mariadb src sqlite/host LICENSE NOTICE | tar x -C "$src"
	for s in $SERIES; do
		docker run --rm --cpus 4 --entrypoint bash -e SERIES="$s" -e VER="$ver" -e HOST_UID="$(id -u)" \
			-v "$REPO/packaging/mariadb:/pkg:ro" -v "$src:/src:ro" -v "$work:/out" "mariadb:$s" /pkg/container.sh build
	done
	mkdir -p "$work/$name/sql" && cp -r "$work/lib" "$work/$name/lib"
	cp "$src/mariadb/sql/functions.sql" "$src/mariadb/sql/sift.sql" "$work/$name/sql/"
	cp "$src/LICENSE" "$src/NOTICE" "$work/$name/"
	cp "$src/mariadb/README.md" "$work/$name/README.md"
	cat "$work"/lib/*/BUILD-INFO > "$work/$name/BUILD-INFO"
	# reproducible: sorted names, the commit time, no owner, no gzip name or time
	tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner -C "$work" -cf - "$name" | gzip -n -9 > "$out/$name.tar.gz"
	echo "built $out/$name.tar.gz"; cat "$work/$name/BUILD-INFO"
}

test_tarball() { # test_tarball TARBALL [SERIES...]
	local tarball base ver s; tarball=$(realpath "$1"); shift
	base=$(basename "$tarball" .tar.gz); ver=${base#sift-mariadb-}; ver=${ver%-linux-x86_64}
	for s in ${*:-$SERIES}; do
		docker run --rm --entrypoint bash -e SERIES="$s" -e VER="$ver" -e TARBALL="$(basename "$tarball")" \
			-v "$REPO/packaging/mariadb:/pkg:ro" -v "$REPO/mariadb:/mdb:ro" -v "$(dirname "$tarball"):/out:ro" "mariadb:$s" /pkg/container.sh test
	done
}

all() { # all [REF [OUTDIR]]
	local ref=${1:-HEAD} out=${2:-$REPO/dist}
	rm -f "$out"/sift-mariadb-*.tar.gz
	build "$ref" "$out"
	test_tarball "$out"/sift-mariadb-*.tar.gz
	(cd "$out" && sha256sum sift-mariadb-*.tar.gz > SHA256SUMS && cat SHA256SUMS)
}

case ${1:-all} in
	build)        shift; build "$@" ;;
	test)         shift; test_tarball "$@" ;;
	all)          shift || true; all "$@" ;;
	*)            all "$@" ;;
esac
