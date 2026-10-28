#!/usr/bin/env bash
# Build and smoke-test the PostgreSQL (PGDG) .deb and .rpm of a pg_sift git ref, one pair for each major of PGS, in throwaway docker containers.
# The source is `git archive REF`; the packaging recipes (packaging/) come from this checkout. The package version is the tag
# (v1.1.0 -> 1.1.0); a ref that is not a tag gets default_version.0~git<sha>.
#
#   scripts/package.sh [all] [REF [OUTDIR]]   build deb and rpm of every major (FAMILIES="deb" or "rpm", PGS="18" or "17 19" for fewer), test each on
#                                              every image below, check that none is missing, write OUTDIR/SHA256SUMS
#   PG=18 scripts/package.sh build IMAGE REF OUTDIR  build the package of IMAGE's family (debian/ubuntu: deb; rocky/alma: rpm) for that major
#   scripts/package.sh test IMAGE PACKAGE      install PACKAGE in a fresh IMAGE container, run packaging/smoke.sql, remove it
#   scripts/package.sh verify DIR              every package of PGS and FAMILIES is in DIR (the release publishes only a complete set)
#   scripts/package.sh majors                  PGS as a JSON array (the matrix of the release workflow)
#   scripts/package.sh --selftest              verify on a scratch directory; no literal major in the recipes of packaging/
#
# REF defaults to HEAD, OUTDIR to dist/. PGS is the list of PostgreSQL majors the release carries (17 and 18 from PGDG, 19 from its testing
# channel until it is released). Override the images with BUILD_DEB, BUILD_RPM, TEST_DEB, TEST_RPM (space-separated lists).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PGS=${PGS:-17 18 19}
FAMILIES=${FAMILIES:-deb rpm}
BUILD_DEB=${BUILD_DEB:-debian:trixie}
BUILD_RPM=${BUILD_RPM:-rockylinux/rockylinux:9}
TEST_DEB=${TEST_DEB:-debian:trixie ubuntu:24.04}
TEST_RPM=${TEST_RPM:-rockylinux/rockylinux:9 almalinux:9 almalinux:10}

run() { # run IMAGE OUTDIR MODE [docker args...]: packaging/container.sh MODE in a throwaway container
	local image=$1 out=$2 mode=$3; shift 3
	docker run --rm --cpus 4 -e HOST_UID="$(id -u)" -v "$REPO/packaging:/pkg:ro" -v "$REPO/packaging/net.sh:/net.sh:ro" -v "$out:/out" "$@" "$image" /pkg/container.sh "$mode"
}

build() {
	local image=$1 ref=$2 out; mkdir -p "$3" && out=$(cd "$3" && pwd)
	local ver epoch src; epoch=$(git -C "$REPO" log -1 --format=%ct "$ref")
	ver=$(git -C "$REPO" describe --tags --exact-match "$ref" 2>/dev/null | sed 's/^v//') || true
	[[ -n $ver ]] || ver=$(git -C "$REPO" show "$ref:pg_sift.control" | sed -n "s/^default_version *= *'\(.*\)'/\1/p").0~git$(git -C "$REPO" rev-parse --short "$ref")
	src=$(mktemp -d -p "$TMP")
	git -C "$REPO" archive --format=tar.gz --prefix="pg_sift-$ver/" -o "$src/pg_sift-$ver.tar.gz" "$ref"
	run "$image" "$out" build -e PG="${PG:?}" -e VER="$ver" -e EPOCH="$epoch" -v "$src:/src:ro"
}

pkg_glob() { # FAMILY MAJOR: the file name pattern of that package
	if [[ $1 == deb ]]; then echo "postgresql-$2-pg-sift_*_amd64.deb"; else echo "pg_sift_$2-*.rpm"; fi
}

test_package() {
	local image=$1 pkg; pkg=$(realpath "$2")
	run "$image" "$(dirname "$pkg")" test -e PKG="$(basename "$pkg")"
}

# test_all PACKAGE IMAGE...: the images in parallel (each waits on its own mirrors), their output in the order of the list
test_all() {
	local pkg=$1 image n=0 pids=() rc=0; shift
	for image in "$@"; do test_package "$image" "$pkg" >"$TMP/test.$n" 2>&1 & pids+=($!); n=$((n + 1)); done
	for n in "${!pids[@]}"; do wait "${pids[n]}" || rc=1; cat "$TMP/test.$n"; done
	return $rc
}

verify() {
	local dir=$1 fam pg missing=()
	for pg in $PGS; do for fam in $FAMILIES; do
		compgen -G "$dir/$(pkg_glob "$fam" "$pg")" >/dev/null || missing+=("$(pkg_glob "$fam" "$pg")")
	done; done
	(( ${#missing[@]} == 0 )) || { echo "package.sh: missing in $dir: ${missing[*]}" >&2; return 1; }
}

all() {
	local ref=${1:-HEAD} out=${2:-$REPO/dist} fam pg b t files
	for fam in $FAMILIES; do rm -f "$out"/*."$fam"; done
	for pg in $PGS; do for fam in $FAMILIES; do
		b=BUILD_${fam^^} t=TEST_${fam^^}
		PG=$pg build "${!b}" "$ref" "$out"
		files=("$out"/$(pkg_glob "$fam" "$pg"))
		test_all "${files[0]}" ${!t}
	done; done
	verify "$out"
	(cd "$out" && shopt -s nullglob && "$REPO/scripts/sha256.sh" *.deb *.rpm > SHA256SUMS && cat SHA256SUMS)
}

selftest() {
	local d f pg fam bad=0
	d=$(mktemp -d -p "$TMP")
	for pg in $PGS; do for fam in $FAMILIES; do : > "$d/$(pkg_glob "$fam" "$pg" | sed 's/\*/3.0.0/')"; done; done
	verify "$d" || { echo "FAIL: verify rejects a complete set"; bad=1; }
	f=$(ls "$d" | sed -n '1p'); rm "$d/$f"
	verify "$d" 2>/dev/null && { echo "FAIL: verify accepts a set without $f"; bad=1; }
	# the major is a placeholder (@PG@, %{pgmajor}) in the recipes, never a literal one
	! grep -nE 'postgresql-[0-9]|pgsql-[0-9]|pg_sift_[0-9]' "$REPO"/packaging/debian/* "$REPO"/packaging/pg_sift.spec || { echo "FAIL: a literal PostgreSQL major in packaging/"; bad=1; }
	grep -q @PG@ "$REPO/packaging/debian/control" || { echo "FAIL: packaging/debian/control has no @PG@"; bad=1; }
	(( bad == 0 )) && echo "package selftest: verify names every missing package of PGS ($PGS) and FAMILIES ($FAMILIES); the recipes carry no literal major"
	return $bad
}

majors() { printf '[%s]' "${PGS// /, }"; }

case ${1:-all} in
	build)        shift; build "$@" ;;
	test)         shift; test_package "$@" ;;
	all)          shift || true; all "$@" ;;
	verify)       shift; verify "$@" ;;
	majors)       majors ;;
	--selftest)   selftest ;;
	*)            all "$@" ;;
esac
