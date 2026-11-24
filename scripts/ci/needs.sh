# shellcheck shell=bash
# What a job needs from the runner besides the staged roots: the compiler (needs_toolchain) and the history of the release tags (needs_git_history). The workflow installs
# each only for the shard that holds a job that needs it (`scripts/ci.sh --toolchain`, `--history`). Sourced by scripts/ci.sh; not a script of its own.
# TOOLCHAIN_USE: what a test says when it builds, or reads the tags of the repository (a shallow clone would skip it, silently)
TOOLCHAIN_USE='build_old|old-lib-check|scaling-gate|pg_upgrade|\bmake\b|\bgcc\b|git -C|git (archive|show|tag|describe)'
# HISTORY_USE: what a test says when it reads a release tag or builds a release from it. pg_upgrade.sh builds the old side from a copy of the working tree unless SIFT_PG_OLD_REF names a
# release (pg_upgrade_31, 32 and 33 do), so the test of the plain upgrade reads no tag
HISTORY_USE='old-lib-check|git -C|git (archive|show|tag|describe)|build_old (v[0-9]|"\$ref)'
NO_HISTORY_JOBS=' tap-shell-pg_upgrade '
# needs_toolchain ID: whether the job compiles (the compiler, the headers of the libraries): the fuzz harnesses and the analyzers, a test that builds an older release,
# the 8-page flavour or pg_upgrade's target. Every other job runs on the staged roots of scripts/ci-build.sh
declare -A NEEDS_TOOLCHAIN NEEDS_HISTORY
needs_toolchain() {
	[[ -v NEEDS_TOOLCHAIN[$1] ]] || { _needs_toolchain "$1" && NEEDS_TOOLCHAIN[$1]=0 || NEEDS_TOOLCHAIN[$1]=1; }
	return "${NEEDS_TOOLCHAIN[$1]}"
}
# needs_git_history ID: whether the job reads a release tag or builds an older release from it. The released libraries build on PostgreSQL 18 only (scripts/old-lib-check.sh: 17 cannot hold
# two builds, no release builds on 19), so no other major unshallows the clone
needs_git_history() {
	(( PG_MAJOR == 18 )) || return 1
	[[ -v NEEDS_HISTORY[$1] ]] || { _needs_history "$1" && NEEDS_HISTORY[$1]=0 || NEEDS_HISTORY[$1]=1; }
	return "${NEEDS_HISTORY[$1]}"
}
# uses_toolchain FILE, uses_history FILE: the code of the test (not its comments, which say "make" and "pg_upgrade" of tests that do neither) builds or reads the tags
uses_toolchain() { grep -vE '^[[:space:]]*#' "$1" | grep -E "$TOOLCHAIN_USE" >/dev/null; }
uses_history() { grep -vE '^[[:space:]]*#' "$1" | grep -E "$HISTORY_USE" >/dev/null; }
# test_file ID: the script of a TAP job
test_file() {
	local f=${1#tap-shell-}
	case $1 in
		tap-shell-*) [[ -f $REPO/test/t/$f.sh ]] || f=${f%-*}; echo "$REPO/test/t/$f.sh" ;;
		tap-*) echo "$REPO/test/t/${1#tap-}.pl" ;;
	esac
}
_needs_toolchain() {
	case $1 in
		fuzz|static|valgrind*|assert|massif|asan-leaks) return 0 ;;
		tap-*) uses_toolchain "$(test_file "$1")" ;;
		*) return 1 ;;
	esac
}
_needs_history() {
	[[ $NO_HISTORY_JOBS != *" $1 "* ]] || return 1
	case $1 in
		tap-*) uses_history "$(test_file "$1")" ;;
		*) return 1 ;;
	esac
}
