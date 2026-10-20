#!/usr/bin/env bash
# Build and smoke-test the release tarball of the SQLite extension (sift-sqlite-VERSION-linux-x86_64.tar.gz: sift.so, LICENSE,
# NOTICE, README.md) for a git ref, in throwaway docker containers. The source is `git archive REF`; the recipe (packaging/sqlite/)
# and the smoke test (sqlite/tests/smoke.py) come from this checkout. The version is the tag (v1.3.0 -> 1.3.0) and must equal
# sqlite/VERSION at the tag; a ref that is not a tag gets sqlite/VERSION-git<sha>.
#
#   scripts/package-sqlite.sh [all] [REF [OUTDIR]]   build, test on every image below, write OUTDIR/SHA256SUMS
#   scripts/package-sqlite.sh build REF OUTDIR        build only
#   scripts/package-sqlite.sh test TARBALL [IMAGE...] smoke-test TARBALL in a fresh container of each IMAGE (default: TEST_IMAGES);
#                                                     the image "host" tests on this machine (its sqlite3 and python3)
#
# REF defaults to HEAD, OUTDIR to dist/. The build image carries the glibc the extension is built against (its baseline: the
# build fails on a newer symbol); override it with BUILD_IMAGE and the test images with TEST_IMAGES (space-separated). The extension
# refuses SQLite older than 3.38, so the test images are the ones whose system SQLite is newer (not RHEL 9 or Ubuntu 22.04).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD_IMAGE=${BUILD_IMAGE:-debian:bookworm}
TEST_IMAGES=${TEST_IMAGES:-debian:bookworm ubuntu:24.04 almalinux:10}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

version() { # version REF: the tag without "v" (which must match sqlite/VERSION), else VERSION-git<sha>
	local ref=$1 file tag
	file=$(git -C "$REPO" show "$ref:sqlite/VERSION" 2>/dev/null) || { echo "$ref has no sqlite/VERSION: it predates the SQLite extension" >&2; return 1; }
	if tag=$(git -C "$REPO" describe --tags --exact-match "$ref" 2>/dev/null); then
		[[ ${tag#v} == "$file" ]] || { echo "tag $tag but sqlite/VERSION is $file: set sqlite/VERSION before tagging" >&2; return 1; }
		echo "$file"
	else
		echo "$file-git$(git -C "$REPO" rev-parse --short "$ref")"
	fi
}

build() { # build REF OUTDIR
	local ref=$1 out ver epoch src; mkdir -p "$2" && out=$(cd "$2" && pwd)
	ver=$(version "$ref"); epoch=$(git -C "$REPO" log -1 --format=%ct "$ref")
	src=$(mktemp -d -p "$TMP")
	git -C "$REPO" archive "$ref" sqlite src scripts LICENSE NOTICE | tar x -C "$src"
	docker run --rm --cpus 4 -e VER="$ver" -e EPOCH="$epoch" -e HOST_UID="$(id -u)" \
		-v "$REPO/packaging/sqlite:/pkg:ro" -v "$src:/src:ro" -v "$out:/out" "$BUILD_IMAGE" /pkg/container.sh build
	echo "built $out/sift-sqlite-$ver-linux-x86_64.tar.gz"
}

test_tarball() { # test_tarball TARBALL [IMAGE...]
	local tarball base ver image dir images; tarball=$(realpath "$1"); shift
	base=$(basename "$tarball" .tar.gz); ver=${base#sift-sqlite-}; ver=${ver%-linux-x86_64}; images=${*:-$TEST_IMAGES}
	for image in $images; do
		if [[ $image == host ]]; then
			dir=$(mktemp -d -p "$TMP"); tar xzf "$tarball" -C "$dir"
			echo "== host: sqlite $(sqlite3 --version 2>/dev/null | cut -d' ' -f1 || true)"
			python3 -I "$REPO/sqlite/tests/smoke.py" "$dir/$base/sift.so" "$ver"
		else
			docker run --rm -e VER="$ver" -e TARBALL="$(basename "$tarball")" -v "$REPO/packaging/sqlite:/pkg:ro" \
				-v "$REPO/sqlite/tests/smoke.py:/smoke.py:ro" -v "$(dirname "$tarball"):/out:ro" "$image" /pkg/container.sh test
		fi
	done
}

all() { # all [REF [OUTDIR]]
	local ref=${1:-HEAD} out=${2:-$REPO/dist}
	rm -f "$out"/sift-sqlite-*.tar.gz
	build "$ref" "$out"
	test_tarball "$out"/sift-sqlite-*.tar.gz
	(cd "$out" && sha256sum sift-sqlite-*.tar.gz > SHA256SUMS && cat SHA256SUMS)
}

case ${1:-all} in
	build)        shift; build "$@" ;;
	test)         shift; test_tarball "$@" ;;
	all)          shift || true; all "$@" ;;
	*)            all "$@" ;;
esac
