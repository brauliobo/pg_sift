#!/usr/bin/env bash
# TAP suite (test/t) on PostgreSQL::Test::Cluster from the server's pgxs tree.
#
#   scripts/tap.sh [t/NNN_name.pl...]     (shell TAP tests: scripts/pgtest.sh tap)
#
# Nodes load sift from the scripts/pgtest.sh staging root (run
# `scripts/pgtest.sh install` first) through TEMP_CONFIG, under ASan when that
# build is sanitized. IPC::Run, required by PostgreSQL::Test::Utils and missing
# on this host, is fetched once from CPAN (sha256 pinned) into
# ${XDG_CACHE_HOME:-~/.cache}/sift/perl5. Logs: $SIFT_TMP/tap/log. Every node is under the leak gate of
# scripts/leakcheck.sh (test/t/SiftTest.pm); the node logs are scanned when prove is done. Exit 1 on failure.
# PG_TEST_PORT_DIR (default $SIFT_TMP/tap) and INITDB_TEMPLATE (a data directory the nodes are copied from) pass through to the nodes,
# so that runs side by side share their port locks and one initdb (scripts/ci.sh).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
eval "$("$REPO/scripts/pgtest.sh" env)"
TAP=${SIFT_TMP:-/tmp/sift-arch}/tap
PGXS_SRC=$(dirname "$(pg_config --pgxs)")/..
PERL5=${XDG_CACHE_HOME:-$HOME/.cache}/sift/perl5
IPC_RUN=IPC-Run-20260402.0
IPC_RUN_SHA256=d6a326a8b1ffb19495dea4ff5a09ef138bf3562ab1b069d3fb7efbc77b9f6aca

[[ -f $SIFT_ROOT/.flavor ]] || { echo "tap: run scripts/pgtest.sh install first" >&2; exit 1; }
# Homebrew's PostgreSQL ships no src/test/perl: the Perl tests cannot run against it. Linux packages and source builds have it, so a missing module is a failure there.
if [[ ! -f $PGXS_SRC/test/perl/PostgreSQL/Test/Cluster.pm && $(uname -s) == Darwin ]]; then
	echo "tap: skipped, this PostgreSQL has no PostgreSQL::Test::Cluster (no src/test/perl in $PGXS_SRC): the Perl TAP tests need a server with its test modules, e.g. a source build"
	exit 0
fi

if ! perl -I"$PERL5" -MIPC::Run -e1 2>/dev/null; then
	dl=$(mktemp -d)
	trap 'rm -rf "$dl"' EXIT
	curl -fsSL -o "$dl/ipc-run.tgz" "https://cpan.metacpan.org/authors/id/T/TO/TODDR/$IPC_RUN.tar.gz"
	echo "$IPC_RUN_SHA256  $dl/ipc-run.tgz" | "$REPO/scripts/sha256.sh" -c --quiet
	tar xzf "$dl/ipc-run.tgz" -C "$dl" "$IPC_RUN/lib"
	mkdir -p "$PERL5" && cp -r "$dl/$IPC_RUN/lib/." "$PERL5/"
fi

rm -rf "$TAP" && mkdir -p "$TAP"
cat >"$TAP/sift.conf" <<CONF
$("$REPO/scripts/pgtest.sh" conf)
max_wal_size = 256MB
CONF
# the sanitizer runtime goes into the servers only (SiftTest.pm preloads SIFT_PRELOAD), not into perl, psql or initdb
[[ -z ${SIFT_SANITIZER:-} ]] || export ${SIFT_SANITIZER/LD_PRELOAD=/SIFT_PRELOAD=}

cd "$REPO/test"
(( $# )) || set -- t/*.pl
rc=0
SIFT_TMP=$TAP TESTDATADIR=$TAP/data TESTLOGDIR=$TAP/log PG_TEST_PORT_DIR=${PG_TEST_PORT_DIR:-$TAP} TEMP_CONFIG=$TAP/sift.conf SIFT_REPO=$REPO \
	PG_REGRESS=$PGXS_SRC/test/regress/pg_regress \
	prove ${PROVE_FLAGS:-} -I "$PGXS_SRC/test/perl" -I "$PERL5" "$@" || rc=$?
"$REPO/scripts/leakcheck.sh" scan "$TAP" || rc=1
exit $rc
