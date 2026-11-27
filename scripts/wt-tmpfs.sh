#!/usr/bin/env bash
# Throwaway checkouts in RAM: a worktree, its test cluster and its build directories live in tmpfs (/tmp) and write nothing to the disk.
#   scripts/wt-tmpfs.sh create [--clone] NAME [REF]   /tmp/sift-wt/NAME at REF (default HEAD): a git worktree of this repository, or with --clone a shared clone with a git
#                                                     directory of its own; prints the exports to eval: SIFT_WT, SIFT_TMP (its cluster, scripts/pgtest.sh) and the compiler cache in RAM
#   scripts/wt-tmpfs.sh clean NAME                    stop and remove its cluster, its build directories (scripts/pgtest.sh keeps them in /tmp/sift-build/NAME-<hash>) and the checkout
#   scripts/wt-tmpfs.sh status                        the use of /tmp and /dev/shm, the checkouts, builds and clusters with their sizes, the hits of the compiler cache
#   scripts/wt-tmpfs.sh prune [--dry-run] [--verbose] remove what no run uses any more: the clusters whose checkout is gone (unless a postmaster runs in them), the builds whose
#                                                     checkout is gone or that sat idle SIFT_BUILD_TTL_MIN minutes (60) and no process uses, the kept budget failures past their bounds
#                                                     (scripts/budget.sh prune), the dead perf gate runs and stale templates, the orphaned PostgreSQL DSM segments (scripts/tmpfs-reap.py);
#                                                     past 45% of /dev/shm used, the compiler cache is trimmed to its cap (at most every 30 minutes)
#   scripts/wt-tmpfs.sh auto-prune                    a prune in the background when /tmp or /dev/shm is past 45% used, at most every 10 minutes: create and scripts/pgtest.sh install run it
#   scripts/wt-tmpfs.sh timer                         installs and starts the user systemd timer that prunes every 15 minutes (sift-tmpfs-prune.timer) with the script of
#                                                     the main checkout of this repository: a throwaway worktree does not outlive the tmpfs
#   scripts/wt-tmpfs.sh --selftest
# SIFT_WT_ROOT, SIFT_PG_ROOT, SIFT_BUILD_ROOT and SIFT_SHM_ROOT move the four directories (default /tmp/sift-wt, /tmp/sift-pg, /tmp/sift-build, /dev/shm).
set -euo pipefail
REPO=${SIFT_WT_REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WT=${SIFT_WT_ROOT:-/tmp/sift-wt} PG=${SIFT_PG_ROOT:-/tmp/sift-pg} BUILD=${SIFT_BUILD_ROOT:-/tmp/sift-build} SHM=${SIFT_SHM_ROOT:-/dev/shm}
HERE=$(cd "$(dirname "$0")" && pwd)
PRUNE_PCT=${SIFT_PRUNE_PCT:-45}
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
	auto_prune
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

# due STAMP MINUTES: true, and the stamp renewed, when the stamp is older than MINUTES or missing
due() { [[ -z $(find "$1" -mmin -"$2" 2>/dev/null) ]] && touch "$1"; }
full() { local d; for d in "$WT" "$SHM"; do [[ ! -d $d ]] || (( $(used_pct "$d") < PRUNE_PCT )) || return 0; done; return 1; }

prune() {
	local d dry= verbose=
	while (( $# )); do case $1 in --dry-run) dry=--dry-run ;; --verbose) verbose=--verbose ;; *) die "prune: unknown option $1" ;; esac; shift; done
	for d in "$PG"/*/; do
		d=${d%/}
		[[ ! -d $d || -d $WT/${d##*/} ]] && continue
		live "$d" || { echo "prune: ${dry:+would remove }$d"; [[ -n $dry ]] || rm -rf "$d"; }
	done
	[[ -n $dry || ! -d $SHM/sift-budget-fail ]] || "$HERE/budget.sh" prune "$SHM/sift-budget-fail"
	python3 -I "$HERE/tmpfs-reap.py" --builds "$BUILD" --perf "${SIFT_PERF_CACHE:-$SHM/sift-perf-gate}" --shm "$SHM" $dry $verbose
	if [[ -z $dry && -d $SHM/sift-ccache ]] && command -v ccache >/dev/null && (( $(used_pct "$SHM") >= PRUNE_PCT )) && due "$SHM/sift-ccache/.trimmed" 30; then
		echo "prune: $SHM is $(used_pct "$SHM")% used: the compiler cache is trimmed to $(ccache_ram_maxsize)"
		CCACHE_DIR=$SHM/sift-ccache CCACHE_MAXSIZE=$(ccache_ram_maxsize) ccache -c >/dev/null
	fi
	return 0
}

auto_prune() {
	mkdir -p "$WT" 2>/dev/null || return 0
	full && due "$WT/.auto-prune" 10 || return 0
	( flock -n 9 || exit 0; "$0" prune ) 9>"$WT/.prune.lock" </dev/null >/dev/null 2>&1 &
}

timer() {
	local dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user main
	systemctl --user show-environment >/dev/null 2>&1 || die "timer: no systemd user manager runs here"
	main=$(dirname "$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir)")
	[[ -x $main/scripts/wt-tmpfs.sh ]] || die "timer: $main has no scripts/wt-tmpfs.sh"
	mkdir -p "$dir"
	printf '[Unit]\nDescription=Remove the tmpfs leftovers of the sift runs\n\n[Service]\nType=oneshot\nNice=10\nIOSchedulingClass=idle\nExecStart=%s prune\n' "$main/scripts/wt-tmpfs.sh" >"$dir/sift-tmpfs-prune.service"
	printf '[Unit]\nDescription=Remove the tmpfs leftovers of the sift runs every 15 minutes\n\n[Timer]\nOnBootSec=5min\nOnUnitActiveSec=15min\n\n[Install]\nWantedBy=timers.target\n' >"$dir/sift-tmpfs-prune.timer"
	systemctl --user daemon-reload
	systemctl --user enable --now sift-tmpfs-prune.timer
	systemctl --user list-timers sift-tmpfs-prune.timer --no-pager
}

selftest() {
	local t out; t=$(mktemp -d); trap "rm -rf '$t'" EXIT
	git init -q "$t/repo" && git -C "$t/repo" -c user.name=t -c user.email=t@example.com commit -q --allow-empty -m base
	export SIFT_WT_REPO=$t/repo SIFT_WT_ROOT=$t/wt SIFT_PG_ROOT=$t/pg SIFT_BUILD_ROOT=$t/build SIFT_SHM_ROOT=$t/shm SIFT_PRUNE_PCT=101
	unset SIFT_PERF_CACHE
	out=$("$0" create a); [[ $out == *"export SIFT_WT=$t/wt/a SIFT_TMP=$t/pg/a"* && -f $t/wt/a/.git ]] || die "selftest: create made no worktree: $out"
	"$0" create a 2>/dev/null && die "selftest: create over an existing checkout"
	"$0" create --clone b >/dev/null; [[ -d $t/wt/b/.git ]] || die "selftest: --clone made no clone"
	mkdir -p "$t/pg/a" "$t/pg/b" "$t/pg/gone" "$t/build/a-0123abcd" "$t/build/gone-89abcdef" "$t/build/b-0123abcd"
	printf %s "$t/nowhere" >"$t/build/gone-89abcdef/.repo"; printf %s "$t/wt/a" >"$t/build/a-0123abcd/.repo"
	[[ $("$0" status) == *"$t/wt/a"* ]] || die "selftest: status does not list the checkout"
	mkdir -p "$t/shm/sift-budget-fail/old" "$t/shm/sift-budget-fail/new" "$t/shm/sift-perf-gate/tpl-old" "$t/build/idle-01234567/flavor"
	touch -d '-2 hours' "$t/shm/sift-budget-fail/old" "$t/shm/sift-perf-gate/tpl-old" "$t/build/idle-01234567/flavor" "$t/build/idle-01234567"
	printf x >"$t/shm/PostgreSQL.12345"; touch -d '-1 hour' "$t/shm/PostgreSQL.12345"
	out=$("$0" prune --dry-run)
	[[ $out == *"would remove $t/pg/gone"* && $out == *"would remove $t/build/idle-01234567"* && $out == *"would remove $t/shm/PostgreSQL.12345"* ]] \
		|| die "selftest: prune --dry-run does not name what prune removes: $out"
	[[ -e $t/pg/gone && -e $t/build/gone-89abcdef && -e $t/build/idle-01234567 && -e $t/shm/sift-budget-fail/old && -e $t/shm/sift-perf-gate/tpl-old && -e $t/shm/PostgreSQL.12345 ]] \
		|| die "selftest: prune --dry-run removed something"
	"$0" prune >/dev/null
	[[ ! -e $t/build/idle-01234567 && ! -e $t/shm/sift-budget-fail/old && -d $t/shm/sift-budget-fail/new && ! -e $t/shm/sift-perf-gate/tpl-old && ! -e $t/shm/PostgreSQL.12345 ]] \
		|| die "selftest: prune left an idle build, an old budget failure, a stale perf template or an orphaned segment, or removed a new failure"
	"$0" auto-prune; [[ ! -e $t/wt/.auto-prune ]] || die "selftest: auto-prune ran under its threshold"
	SIFT_PRUNE_PCT=0 "$0" auto-prune; [[ -e $t/wt/.auto-prune ]] || die "selftest: auto-prune did not run past its threshold"
	touch -d '-2 min' "$t/wt/.auto-prune"; SIFT_PRUNE_PCT=0 "$0" auto-prune
	[[ -n $(find "$t/wt/.auto-prune" -mmin +1) ]] || die "selftest: auto-prune ran twice in 10 minutes"
	flock "$t/wt/.prune.lock" true
	mkdir -p "$t/bin"; printf '#!/bin/sh\necho "$*" >>"%s/systemctl.log"\n' "$t" >"$t/bin/systemctl"; chmod +x "$t/bin/systemctl"
	mkdir -p "$t/repo/scripts"; cp "$0" "$t/repo/scripts/"
	PATH=$t/bin:$PATH XDG_CONFIG_HOME=$t/config "$0" timer >/dev/null
	[[ $(<"$t/config/systemd/user/sift-tmpfs-prune.service") == *"ExecStart=$t/repo/scripts/wt-tmpfs.sh prune"* \
		&& $(<"$t/config/systemd/user/sift-tmpfs-prune.timer") == *'OnUnitActiveSec=15min'* && $(<"$t/systemctl.log") == *'--user enable --now sift-tmpfs-prune.timer'* ]] \
		|| die "selftest: timer does not install and start a 15 minute timer that prunes with the main checkout"
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
	prune) shift; prune "$@" ;;
	auto-prune) auto_prune ;;
	timer) timer ;;
	--selftest) selftest ;;
	*) sed -n '2,15p' "$0"; exit 1 ;;
esac
