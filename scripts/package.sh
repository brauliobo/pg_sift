#!/usr/bin/env bash
# Build and smoke-test the PostgreSQL 18 (PGDG) .deb and .rpm of a pg_sift git ref in throwaway docker containers.
# The source is `git archive REF`; the packaging recipes (packaging/) come from this checkout. The package version is the tag
# (v1.1.0 -> 1.1.0); a ref that is not a tag gets default_version.0~git<sha>.
#
#   scripts/package.sh [all] [REF [OUTDIR]]   build deb and rpm (FAMILIES="deb" or "rpm" for one), test them on every image below, write OUTDIR/SHA256SUMS
#   scripts/package.sh build IMAGE REF OUTDIR  build the package of IMAGE's family (debian/ubuntu: deb; rocky/alma: rpm)
#   scripts/package.sh test IMAGE PACKAGE      install PACKAGE in a fresh IMAGE container, run packaging/smoke.sql, remove it
#
# REF defaults to HEAD, OUTDIR to dist/. Override the images with BUILD_DEB, BUILD_RPM, TEST_DEB, TEST_RPM (space-separated lists).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
BUILD_DEB=${BUILD_DEB:-debian:trixie}
BUILD_RPM=${BUILD_RPM:-rockylinux/rockylinux:9}
TEST_DEB=${TEST_DEB:-debian:trixie ubuntu:24.04}
TEST_RPM=${TEST_RPM:-rockylinux/rockylinux:9 almalinux:9 almalinux:10}

run() { # run IMAGE OUTDIR MODE [docker args...]: packaging/container.sh MODE in a throwaway container
	local image=$1 out=$2 mode=$3; shift 3
	docker run --rm --cpus 4 -e HOST_UID="$(id -u)" -v "$REPO/packaging:/pkg:ro" -v "$out:/out" "$@" "$image" /pkg/container.sh "$mode"
}

build() {
	local image=$1 ref=$2 out; mkdir -p "$3" && out=$(cd "$3" && pwd)
	local ver epoch src; epoch=$(git -C "$REPO" log -1 --format=%ct "$ref")
	ver=$(git -C "$REPO" describe --tags --exact-match "$ref" 2>/dev/null | sed 's/^v//') || true
	[[ -n $ver ]] || ver=$(git -C "$REPO" show "$ref:pg_sift.control" | sed -n "s/^default_version *= *'\(.*\)'/\1/p").0~git$(git -C "$REPO" rev-parse --short "$ref")
	src=$(mktemp -d -p "$TMP")
	git -C "$REPO" archive --format=tar.gz --prefix="pg_sift-$ver/" -o "$src/pg_sift-$ver.tar.gz" "$ref"
	run "$image" "$out" build -e VER="$ver" -e EPOCH="$epoch" -v "$src:/src:ro"
}

test_package() {
	local image=$1 pkg; pkg=$(realpath "$2")
	run "$image" "$(dirname "$pkg")" test -e PKG="$(basename "$pkg")"
}

all() {
	local ref=${1:-HEAD} out=${2:-$REPO/dist} fam image b t
	for fam in ${FAMILIES:-deb rpm}; do
		b=BUILD_${fam^^} t=TEST_${fam^^}
		rm -f "$out"/*."$fam"
		build "${!b}" "$ref" "$out"
		for image in ${!t}; do test_package "$image" "$out"/*."$fam"; done
	done
	(cd "$out" && shopt -s nullglob && sha256sum *.deb *.rpm > SHA256SUMS && cat SHA256SUMS)
}

case ${1:-all} in
	build)        shift; build "$@" ;;
	test)         shift; test_package "$@" ;;
	all)          shift || true; all "$@" ;;
	*)            all "$@" ;;
esac
