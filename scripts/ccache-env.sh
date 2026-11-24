#!/usr/bin/env bash
# The compiler cache of the builds, in RAM: sourced by scripts/pgtest.sh and scripts/wt-tmpfs.sh.
#   ccache_ram_env [BASEDIR]   exports the CCACHE_* defaults that let every checkout of the machine share one cache (hits across worktrees, no writes to the disk),
#                              unless the caller names a CCACHE_DIR of its own, ccache is not installed, or /dev/shm is missing, read-only or has less than 4 GiB free
#                              (macOS, a container, a CI runner: the builds then use ccache's own defaults). BASEDIR (default the current directory): the paths under it are hashed relative to it
ccache_ram_env() {
	[[ -z ${CCACHE_DIR:-} ]] && command -v ccache >/dev/null || return 0
	[[ -w /dev/shm && $(df -Pk /dev/shm | awk 'NR == 2 { print $4 }') -ge 4194304 ]] || return 0
	export CCACHE_DIR=/dev/shm/sift-ccache CCACHE_MAXSIZE=10G CCACHE_COMPRESS=1 CCACHE_COMPRESSLEVEL=1 CCACHE_NOHASHDIR=1
	export CCACHE_BASEDIR=${1:-$PWD} CCACHE_SLOPPINESS=time_macros,include_file_mtime,file_stat_matches
}
