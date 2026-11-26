#!/usr/bin/env bash
# tap: pooled
# The packaging and the 5.1 to 6.0 update: the SQL parts are built by one recipe however many jobs make runs, the compiler of PGXS (not the host) names the target of the code generation flags,
# every recipe installs python3, the preflight names a view over sift.table_paths_maintained (the update drops it), the update leaves no stub of a function 6.0 removed, ddl_hooks_enable(true)
# sweeps the policy tables, and packaging/smoke.sql passes from 5.1 and from 6.0.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

REPO=${SIFT_REPO:?}
q() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock pk)" -p "$(port pk)" -U postgres -d "$db" "${@/#/--command=}"; }
smoke() { psql -X -q -v ON_ERROR_STOP=1 -v from="$2" -v to=6.0 -h "$(node_sock pk)" -p "$(port pk)" -U postgres -d "$1" -f "$REPO/packaging/smoke.sql" 2>&1; }
uc() { PGHOST=$(node_sock pk) PGPORT=$(port pk) PGUSER=postgres "$REPO/scripts/upgrade-check.sh" "$@" 2>/dev/null; }

plan 23

# make -jN: the generated scripts have one recipe, not one for each of them
out=$(make -C "$REPO" -n -B -j8 update-sql 2>&1)
check "make -j8 update-sql runs the builder of the SQL parts once" test "$(grep -c 'scripts/sql-parts.sh build' <<<"$out")" == 1

# the flags of the code generation follow the compiler PGXS names: a compiler that targets aarch64 gets the Arm baseline whatever the host is
cc=$TAPDIR/cc-aarch64
mkdir -p "$TAPDIR"
printf '#!/bin/sh\n[ "$1" = -dumpmachine ] && { echo aarch64-linux-gnu; exit 0; }\nexec cc "$@"\n' >"$cc"
chmod +x "$cc"
out=$(printf '$(info ISA=$(SIFT_BASE_ISA))\n' | make -C "$REPO" -s -n -f Makefile -f - update-sql CC="$cc" 2>&1 | sed -n 's/^ISA=//p')
check "the baseline of the code generation is the one of the compiler's target" test "$out" == "-march=armv8-a -mtune=generic"
rm -f "$cc"

# every package recipe installs python3: scripts/sql-parts.sh needs it to build the update scripts
for f in packaging/docker/Dockerfile packaging/debian/control packaging/container.sh packaging/pg_sift.spec packaging/PKGBUILD packaging/pgxman.yaml; do
	check "$f installs python3" grep -q -E '\bpython3?\b' "$REPO/$f"
done
check "the container image of Debian Trixie installs libcurl4t64 where libcurl4 is gone" grep -q 'libcurl4t64' "$REPO/packaging/docker/Dockerfile"
check "the container images are built for the majors the official image has by default" grep -q 'tags=(17 18)' "$REPO/packaging/docker/build.sh"
check "the Homebrew formula lists curl before icu4c" test "$(grep -n -E 'depends_on "(curl|icu4c)"' "$REPO/Formula/pg_sift.rb" | sed 's/.*"\(.*\)"/\1/' | tr '\n' ' ')" == "curl icu4c "

# the Vulkan header is for a build with Vulkan
check "agg/vk/spv.h is included only in a build with Vulkan" test "$(awk '/^#ifdef SIFT_VULKAN/ {on = 1} /#include "agg\/vk\/spv.h"/ {print on + 0}' "$REPO/src/agg/vk/dev.c")" == 1

node_init pk 1 "autovacuum = off"
node_start pk || bail "start failed"
q "$(node_db pk)" "CREATE DATABASE old" "CREATE DATABASE fresh" "CREATE DATABASE smoke51" "CREATE DATABASE smoke60" >/dev/null

# the preflight names what the update stops on: a view over the view that the update drops and makes again
q old "CREATE EXTENSION pg_sift VERSION '5.1'" "CREATE VIEW public.paths_v AS SELECT * FROM sift.table_paths_maintained" >/dev/null || bail "the 5.1 extension cannot be created"
out=$(uc old)
check "a view over sift.table_paths_maintained is a blocker of the update" test -n "$(awk '$1 == "blocker" && $2 == "public.paths_v"' <<<"$out")"
check "the update stops on it" grep -q 'table_paths_maintained' <<<"$(q old "ALTER EXTENSION pg_sift UPDATE" 2>&1)"
q old "DROP VIEW public.paths_v" "ALTER EXTENSION pg_sift UPDATE" >/dev/null
cur_ver=$(sed -n "s/^default_version *= *'\(.*\)'$/\1/p" "$REPO/pg_sift.control")
check "without the view the update runs to $cur_ver" test "$(q old "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" = "$cur_ver"
check "after the update no dependency is a blocker" test -z "$(awk '$1 == "blocker" && $3 == "dependency"' <<<"$(uc old)")"

# the functions 6.0 removed and left behind in pg_proc of a database that was updated: gone; the trigger function that 6.0 still implements stays
stubs="SELECT count(*) FROM pg_proc WHERE pronamespace = 'sift'::regnamespace AND proname IN ('_col_value_store_begin', '_col_value_store_end')"
check "the update leaves no stub of the value store helpers" test "$(q old "$stubs")" == 0
q fresh "CREATE EXTENSION pg_sift" >/dev/null
check "a new 6.0 database has none either" test "$(q fresh "$stubs")" == 0
check "the trigger function of track_changes is still there" test "$(q old "SELECT count(*) FROM pg_proc WHERE pronamespace = 'sift'::regnamespace AND proname = 'col_track_old_rows'")" == 1

# ddl_hooks_enable(true) removes the rows of the policy tables that name a relation that is gone, as it does those of the paths and the options
orphans="SELECT (SELECT count(*) FROM sift.projection_defs) + (SELECT count(*) FROM sift.tier_policies) + (SELECT count(*) FROM sift.ttl_policies)"
q fresh "SELECT sift.ddl_hooks_enable(false)" \
	"INSERT INTO sift.projection_defs (rel, name, spec) VALUES (4294000001::oid::sift.tblref, 'p', 'x')" \
	"INSERT INTO sift.tier_policies (relid, uri, move_after, scope_id) VALUES (4294000002::oid::regclass, 'file:///nowhere', '1 day', 4294000002)" \
	"INSERT INTO sift.ttl_policies (relid, col, retention) VALUES (4294000003::oid::sift.tblref, 'c', '1 day')" >/dev/null || bail "the orphans cannot be made"
check "the policy tables hold the three rows of relations that are gone" test "$(q fresh "$orphans")" == 3
q fresh "SELECT sift.ddl_hooks_enable(true)" >/dev/null
check "ddl_hooks_enable(true) removes them" test "$(q fresh "$orphans")" == 0

# the smoke test of the packages: a 5.1 database updated to 6.0, and a new 6.0 one
out=$(smoke smoke51 5.1)
check "packaging/smoke.sql passes from 5.1" test $? -eq 0 -a -z "$(grep -E 'ERROR|ASSERT' <<<"$out")"
out=$(smoke smoke60 6.0)
check "packaging/smoke.sql passes from 6.0" test $? -eq 0 -a -z "$(grep -E 'ERROR|ASSERT' <<<"$out")"
finish
