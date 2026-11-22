#!/usr/bin/env bash
# The build stage of the CI workflow: every library flavor the test shards of a (PostgreSQL major, architecture) install, compiled once, staged as a root each
# (the layout scripts/pgtest.sh install makes with DESTDIR), the checks that need the build tree, and one archive the shards unpack and point
# SIFT_PREBUILT_ROOT, SIFT_PREBUILT_RELEASE and SIFT_PREBUILT_HARDENED at (scripts/pgtest.sh install, release-library-check.sh and hardened.sh then compile nothing).
#
#   scripts/ci-build.sh OUT FLAVOR...        FLAVOR: testing (the test library), release, hardened, asan (the test library with ASan+UBSan)
#   scripts/ci-build.sh --check NAME         the result of a check the testing build ran (isa-baseline, tools-build), from the staged root SIFT_ROOT: the plan's jobs of that name
#   scripts/ci-build.sh --selftest           every flavor has its environment, an unknown one is refused, a build that fails ends with status 1 and the compiler's error
#
# Each flavor is built in a private copy of the tree, side by side (the compiler cache serves what the flavors share). OUT/root-FLAVOR holds the staged library, OUT/sift-bin.tar.zst the
# roots as one archive. The testing flavor also runs scripts/isa_check.sh on its objects and scripts/check-tools-build.sh, which the shards no longer compile for.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=scripts/source-snapshot.sh
source "$REPO/scripts/source-snapshot.sh"

flavor_env() { # FLAVOR: the environment of its build
	case $1 in
		testing) echo "SIFT_TESTING=1 SANITIZE=" ;;
		release) echo "SIFT_TESTING= SANITIZE= SIFT_TEST_HELPERS=1" ;;
		hardened) echo "SIFT_TESTING=1 SANITIZE= SIFT_HARDEN=1" ;;
		asan) echo "SIFT_TESTING=1 SANITIZE=1" ;;
		*) echo "ci-build: unknown flavor $1 (testing, release, hardened, asan)" >&2; return 1 ;;
	esac
}

build_failure() { # FLAVOR LOG: the lines that name the failure (compiler and linker errors, the check that failed, the make rule), then the end of the log
	echo "ci-build: the $1 build failed; $2:"
	grep -nE 'error:|undefined reference|undefined symbol|linker command failed|fatal error|ld: |does not build|Error [0-9]+$|\*\*\*|^(check-|ci-build|scripts/)[^ ]*: ' "$2" | head -n 60 || true
	echo "ci-build: the end of $2:"
	tail -n 20 "$2"
}

stage_flavor() { # OUT FLAVOR: runs as its own process (--stage), where set -e holds; in a `||` list it would be off and a failed build would go on to be staged
	local out=$1 f=$2 work=$1/work-$2
	rm -rf "$work" && mkdir -p "$work/tree"
	copy_git_sources "$REPO" "$work/tree"
	cd "$work/tree"
	# shellcheck disable=SC2046  # the words of flavor_env are assignments
	env $(flavor_env "$f") SIFT_STAGE=1 SIFT_TMP="$work/tmp" SIFT_ROOT="$out/root-$f" SIFT_PORT="${SIFT_PORT:-56100}" SIFT_MAKE_JOBS="$JOBS" scripts/pgtest.sh install
	if [[ $f == testing ]]; then
		mkdir -p "$out/root-$f/.checks"
		scripts/isa_check.sh | tee "$out/root-$f/.checks/isa-baseline"
		scripts/check-tools-build.sh | tee "$out/root-$f/.checks/tools-build"
	fi
	rm -rf "$work"
}

build_flavor() { # OUT FLAVOR
	local out=$1 f=$2
	"$REPO/scripts/ci-build.sh" --stage "$out" "$f" >"$out/build-$f.log" 2>&1 || { build_failure "$f" "$out/build-$f.log"; return 1; }
	echo "ci-build: $f staged in $out/root-$f ($(du -sk "$out/root-$f" | cut -f1) kB)"
}

if [[ ${1:-} == --stage ]]; then
	stage_flavor "$2" "$3"
	exit 0
fi
if [[ ${1:-} == --check ]]; then
	cat "${SIFT_ROOT:?SIFT_ROOT names the staged root}/.checks/${2:?check name}" 2>/dev/null || { echo "ci-build: no result of $2 in $SIFT_ROOT: the build stage did not run it" >&2; exit 1; }
	exit 0
fi
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	for f in testing release hardened asan; do flavor_env "$f" >/dev/null || { echo "selftest: $f has no environment"; exit 1; }; done
	flavor_env nonsense 2>/dev/null && { echo "selftest: an unknown flavor was accepted"; exit 1; }
	{ echo "cc -c a.c"; echo "test/fuzz/fz.c:102:5: error: call to undeclared function 'sift_count'"; seq 1 200 | sed 's/^/noise /'; } >"$d/log"
	[[ $(build_failure selftest "$d/log" | grep -c "call to undeclared function") == 1 ]] || { echo "selftest: the failure report lacks the compiler error that precedes 200 lines of noise"; exit 1; }
	mkdir -p "$d/repo/scripts" "$d/out"
	cp "$REPO/scripts/ci-build.sh" "$REPO/scripts/source-snapshot.sh" "$d/repo/scripts/"
	printf '#!/usr/bin/env bash\necho "stub.c:1:1: error: stub compile failure"\nexit 1\n' >"$d/repo/scripts/pgtest.sh"
	chmod +x "$d/repo/scripts/pgtest.sh"
	git -C "$d/repo" init -q && git -C "$d/repo" add scripts
	"$d/repo/scripts/ci-build.sh" "$d/out" release >"$d/run.log" 2>&1 && { echo "selftest: a build that failed was reported as staged"; exit 1; }
	[[ $(grep -c "error: stub compile failure" "$d/run.log") -ge 1 && ! -e $d/out/sift-bin.tar.zst ]] || { echo "selftest: a failed build does not end with its compiler error and no archive"; cat "$d/run.log"; exit 1; }
	echo "ci-build selftest ok"
	exit 0
fi

out=${1:?usage: scripts/ci-build.sh OUT FLAVOR...}
shift
(( $# )) || { echo "usage: scripts/ci-build.sh OUT FLAVOR..." >&2; exit 1; }
mkdir -p "$out"
for f in "$@"; do flavor_env "$f" >/dev/null; done
JOBS=$(( ${SIFT_MAKE_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)} / $# ))   # the flavors share the cores
(( JOBS >= 1 )) || JOBS=1
export JOBS
pids=()
for f in "$@"; do build_flavor "$out" "$f" & pids+=($!); done
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=1; done
(( rc == 0 )) || exit 1
tar -C "$out" -cf - "${@/#/root-}" | zstd -T0 -3 -q -o "$out/sift-bin.tar.zst" -f
echo "ci-build: $out/sift-bin.tar.zst ($(du -k "$out/sift-bin.tar.zst" | cut -f1) kB): $*"
