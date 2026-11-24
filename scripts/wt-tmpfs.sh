#!/usr/bin/env bash
# Throwaway checkouts in RAM: a worktree, its test cluster and its build directories live in tmpfs (/tmp) and write nothing to the disk.
#   scripts/wt-tmpfs.sh create [--clone] NAME [REF]   /tmp/sift-wt/NAME at REF (default HEAD): a git worktree of this repository, or with --clone a shared clone with a git
#                                                     directory of its own; prints the exports to eval: SIFT_WT, SIFT_TMP (its cluster, scripts/pgtest.sh) and the compiler cache in RAM
#   scripts/wt-tmpfs.sh clean NAME                    stop and remove its cluster, its build directories (scripts/pgtest.sh keeps them in /tmp/sift-build/NAME-<hash>) and the checkout
#   scripts/wt-tmpfs.sh status                        the use of /tmp and /dev/shm, the checkouts, builds and clusters with their sizes, the hits of the compiler cache
#   scripts/wt-tmpfs.sh prune                         remove the clusters and the builds whose checkout is gone, unless a postmaster still runs in the cluster
#   scripts/wt-tmpfs.sh --selftest
# SIFT_WT_ROOT, SIFT_PG_ROOT and SIFT_BUILD_ROOT move the three directories (default /tmp/sift-wt, /tmp/sift-pg, /tmp/sift-build).
set -euo pipefail
REPO=${SIFT_WT_REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WT=${SIFT_WT_ROOT:-/tmp/sift-wt} PG=${SIFT_PG_ROOT:-/tmp/sift-pg} BUILD=${SIFT_BUILD_ROOT:-/tmp/sift-build}
# shellcheck source=scripts/ccache-env.sh
. "$(dirname "$0")/ccache-env.sh"

die() { echo "wt-tmpfs: $*" >&2; exit 1; }
used_pct() { df -P "$1" | awk 'NR == 2 { sub(/%/, "", $5); print $5 }'; }
free_kb() { df -Pk "$1" | awk 'NR == 2 { print $4 }'; }
live() { [[ -f $1/data/postmaster.pid ]] && ps -p "$(head -n1 "$1/data/postmaster.pid")" >/dev/null 2>&1; }
sizes() { local d; for d in "$@"; do [[ ! -d $d ]] || du -sh "$d" | sed 's/^/  /'; done; }

create() {
	local clone= name ref
	[[ ${1:-} != --clone ]] || { clone=1; shift; }
	name=${1:?usage: wt-tmpfs.sh create [--clone] NAME [REF]} ref=${2:-HEAD}
	[[ $name =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "NAME is a plain directory name: $name"
	[[ ! -e $WT/$name ]] || die "$WT/$name exists: scripts/wt-tmpfs.sh clean $name"
	mkdir -p "$WT"
	if (( $(used_pct "$WT") >= 75 || $(free_kb "$WT") < 4194304 )); then
		echo "wt-tmpfs: $WT is $(used_pct "$WT")% used with $(( $(free_kb "$WT") / 1048576 )) GiB free (under 75% and 4 GiB are needed). Checkouts:" >&2
		sizes "$WT"/* >&2
		die "remove the ones you are done with: scripts/wt-tmpfs.sh clean NAME, or scripts/wt-tmpfs.sh prune"
	fi
	if [[ -n $clone ]]; then
		git clone -q --shared --no-checkout "$REPO" "$WT/$name" && git -C "$WT/$name" checkout -q --detach "$ref"
	else
		git -C "$REPO" worktree add -q --detach "$WT/$name" "$ref"
	fi
	echo "export SIFT_WT=$WT/$name SIFT_TMP=$PG/$name"
	( ccache_ram_env "$WT"; env | sed -n 's/^\(CCACHE_[A-Z_]*\)=\(.*\)$/export \1=\2/p' )
}

clean() {
	local name=${1:?usage: wt-tmpfs.sh clean NAME} wt common b
	wt=$WT/$name
	if [[ -x $wt/scripts/pgtest.sh ]]; then SIFT_TMP=$PG/$name "$wt/scripts/pgtest.sh" destroy || true; fi
	! live "$PG/$name" || die "a postmaster still runs in $PG/$name: stop it first"
	rm -rf "${PG:?}/$name"
	for b in "$BUILD/$name"-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]; do rm -rf "$b"; done
	if [[ -f $wt/.git ]]; then
		common=$(git -C "$wt" rev-parse --path-format=absolute --git-common-dir)
		git --git-dir="$common" worktree remove --force "$wt"
		git --git-dir="$common" worktree prune
	else
		rm -rf "${WT:?}/$name"
	fi
}

status() {
	df -h /tmp /dev/shm
	echo "checkouts ($WT):"; sizes "$WT"/*
	echo "builds ($BUILD):"; sizes "$BUILD"/*
	echo "clusters ($PG):"; sizes "$PG"/*
	if command -v ccache >/dev/null; then
		echo "compiler cache (${CCACHE_DIR:-ccache default}):"
		ccache -s | sed -n '/Hits:/p;/Misses:/p;/Cache size/p' | sed -n '1,3p;$p'
	fi
}

prune() {
	local d
	for d in "$PG"/*/; do
		d=${d%/}
		[[ ! -d $d || -d $WT/${d##*/} ]] && continue
		live "$d" || { echo "prune: $d"; rm -rf "$d"; }
	done
	for d in "$BUILD"/*/; do
		d=${d%/}
		[[ -f $d/.repo && ! -d $(<"$d/.repo") ]] && { echo "prune: $d"; rm -rf "$d"; }
	done
	return 0
}

selftest() {
	local t out; t=$(mktemp -d); trap "rm -rf '$t'" EXIT
	git init -q "$t/repo" && git -C "$t/repo" -c user.name=t -c user.email=t@example.com commit -q --allow-empty -m base
	export SIFT_WT_REPO=$t/repo SIFT_WT_ROOT=$t/wt SIFT_PG_ROOT=$t/pg SIFT_BUILD_ROOT=$t/build
	out=$("$0" create a); [[ $out == *"export SIFT_WT=$t/wt/a SIFT_TMP=$t/pg/a"* && -f $t/wt/a/.git ]] || die "selftest: create made no worktree: $out"
	"$0" create a 2>/dev/null && die "selftest: create over an existing checkout"
	"$0" create --clone b >/dev/null; [[ -d $t/wt/b/.git ]] || die "selftest: --clone made no clone"
	mkdir -p "$t/pg/a" "$t/pg/b" "$t/pg/gone" "$t/build/a-0123abcd" "$t/build/gone-89abcdef" "$t/build/b-0123abcd"
	printf %s "$t/nowhere" >"$t/build/gone-89abcdef/.repo"; printf %s "$t/wt/a" >"$t/build/a-0123abcd/.repo"
	[[ $("$0" status) == *"$t/wt/a"* ]] || die "selftest: status does not list the checkout"
	"$0" prune >/dev/null
	[[ ! -e $t/pg/gone && ! -e $t/build/gone-89abcdef && -d $t/pg/a && -d $t/pg/b && -d $t/build/a-0123abcd ]] || die "selftest: prune removed what a checkout still has, or left what none has"
	"$0" clean a
	[[ ! -e $t/wt/a && ! -e $t/pg/a && ! -e $t/build/a-0123abcd && -d $t/wt/b && -d $t/pg/b && -d $t/build/b-0123abcd ]] \
		|| die "selftest: clean left a checkout, a cluster or a build of its own, or removed those of another"
	"$0" clean b
	[[ ! -e $t/wt/b && ! -e $t/pg/b && ! -e $t/build/b-0123abcd ]] || die "selftest: clean left a clone, a cluster or a build"
	[[ -z $(git -C "$t/repo" worktree list | sed 1d) ]] || die "selftest: clean left a worktree registered"
	echo "wt-tmpfs selftest: ok"
}

case ${1:-} in
	create) shift; create "$@" ;;
	clean) clean "${2:-}" ;;
	status) status ;;
	prune) prune ;;
	--selftest) selftest ;;
	*) sed -n '2,9p' "$0"; exit 1 ;;
esac
