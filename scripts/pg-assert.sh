#!/usr/bin/env bash
# PostgreSQL built from source with assertions and memory debugging, for the `assert` and `valgrind` CI lanes (docs/SAFETY.md). A
# distribution server has none of it: --enable-cassert implies MEMORY_CONTEXT_CHECKING (palloc overruns and double frees are caught at
# pfree, which ASan cannot see inside a chunk), CLOBBER_FREED_MEMORY (freed memory is overwritten, so a use after free reads garbage) and
# RANDOMIZE_ALLOCATED_MEMORY (new memory is not zero, so a missing palloc0 shows; the one of the three that needs a -D) and CATCACHE_FORCE_RELEASE (a catalog cache
# entry is freed when its last reference goes, so a syscache tuple used after ReleaseSysCache reads freed memory; also a -D). debug_discard_caches, which cassert enables as a
# setting, reloads the relcache entries (tests cache_discard, verify_relcache). The --valgrind
# variant adds USE_VALGRIND, which makes the memory contexts tell valgrind about every chunk (src/backend/utils/mmgr/memdebug.c).
#
#   scripts/pg-assert.sh [--valgrind] [--major 17|18]     builds when missing and prints the prefix on stdout (make pg-assert)
#
# The pinned minors are the ones of the distribution server this was written against (18.6; 17.11 for the 17 branch), the tarball is
# checked against the sha256 below. Prefix: $SIFT_PG_CACHE (default ${XDG_CACHE_HOME:-~/.cache}/sift/pg)/<version>-assert[-valgrind]; the
# tarballs are kept in $SIFT_PG_CACHE/src. A build takes about five minutes (nice, 4 jobs); it is not repeated while the recipe (version,
# flags) is the same, and concurrent callers wait for each other. The prefix has the pgxs tree, pg_regress, the TAP perl modules, the
# contrib modules the tests use and share/valgrind.supp (src/tools/valgrind.supp of the same tree). Needs: curl, bzip2, gcc, make, flex, bison, perl.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

declare -A VERSION=([18]=18.6 [17]=17.11)
declare -A SHA256=([18.6]=555610c24d53e4316da5b7d3fc25c279d96856d5e0e23ee308c328c5fa881d9f [17.11]=dd27f2b3c59e73ed14aa3324901242bf69a032a6347805f274e6260322d42979)
CONTRIB="amcheck pg_walinspect pg_buffercache"

VALGRIND=${PG_ASSERT_VALGRIND:-}
MAJOR=${PG_ASSERT_MAJOR:-}
while (( $# )); do
	case $1 in
		--valgrind) VALGRIND=1 ;;
		--major) MAJOR=$2; shift ;;
		*) sed -n '2,15p' "$0"; exit 1 ;;
	esac
	shift
done
# the major of the server on PATH, if any (a builder without one gets 18)
[[ -n $MAJOR ]] || MAJOR=$( { pg_config --version 2>/dev/null || true; } | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
[[ -n ${VERSION[${MAJOR:-0}]:-} ]] || MAJOR=18
V=${PG_ASSERT_VERSION:-${VERSION[$MAJOR]}}
SUM=${PG_ASSERT_SHA256:-${SHA256[$V]:-}}
[[ -n $SUM ]] || { echo "pg-assert: no sha256 for PostgreSQL $V (PG_ASSERT_SHA256)" >&2; exit 1; }

CACHE=${SIFT_PG_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/sift/pg}
PREFIX=$CACHE/$V-assert${VALGRIND:+-valgrind}
CPP="-DRANDOMIZE_ALLOCATED_MEMORY -DCATCACHE_FORCE_RELEASE${VALGRIND:+ -DUSE_VALGRIND}"
CFLAGS_="-O1 -ggdb -fno-omit-frame-pointer"
RECIPE="$V sha256=$SUM cassert debug tap CPPFLAGS=[$CPP] CFLAGS=[$CFLAGS_] contrib=[$CONTRIB]"

mkdir -p "$CACHE/src"
exec 9>"$CACHE/.lock"
flock 9
if [[ $(cat "$PREFIX/.recipe" 2>/dev/null) == "$RECIPE" ]]; then echo "$PREFIX"; exit 0; fi

[[ -z $VALGRIND ]] || [[ -e /usr/include/valgrind/memcheck.h || -e /usr/local/include/valgrind/memcheck.h ]] ||
	{ echo "pg-assert: valgrind/memcheck.h not found (install the valgrind development files)" >&2; exit 1; }
TARBALL=$CACHE/src/postgresql-$V.tar.bz2
if ! echo "$SUM  $TARBALL" | "$(dirname "$0")/sha256.sh" -c --quiet 2>/dev/null; then
	curl -fsSL -o "$TARBALL" "https://ftp.postgresql.org/pub/source/v$V/postgresql-$V.tar.bz2"
	echo "$SUM  $TARBALL" | "$(dirname "$0")/sha256.sh" -c --quiet
fi

WORK=$(mktemp -d "${SIFT_TMP:-/tmp}/sift-pg-assert-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
{
	tar -xjf "$TARBALL" -C "$WORK"
	cd "$WORK/postgresql-$V"
	# IPC::Run, which --enable-tap-tests needs, comes from the cache scripts/tap.sh fills
	export PERL5LIB="${XDG_CACHE_HOME:-$HOME/.cache}/sift/perl5${PERL5LIB:+:$PERL5LIB}"
	rm -rf "$PREFIX"
	CPPFLAGS="$CPP" CFLAGS="$CFLAGS_" ./configure --prefix="$PREFIX" --enable-cassert --enable-debug --enable-tap-tests \
		--without-icu --without-readline --without-zlib >configure.log 2>&1 || { tail -n 20 configure.log; exit 1; }
	nice make -j4 -s >make.log 2>&1 || { tail -n 30 make.log; exit 1; }
	nice make -s install >/dev/null
	for c in $CONTRIB; do nice make -s -C contrib/$c install >/dev/null; done
	install -D -m 644 src/tools/valgrind.supp "$PREFIX/share/valgrind.supp"
} >&2
echo "$RECIPE" >"$PREFIX/.recipe"
echo "$PREFIX"
