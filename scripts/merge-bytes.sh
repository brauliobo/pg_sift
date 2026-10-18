#!/usr/bin/env bash
# The index file two builds of sift leave after the same build, merges, churn and VACUUM, compared page by page: a change of the merge
# path must produce the bytes of the one before it (segments are deterministic: same documents, same generations, same blocks).
#
#   scripts/merge-bytes.sh [--rows N] [--build LABEL=DIR]...
#
# The first --build is the reference, e.g. --build old=/tmp/1.4.0 --build new=. In throwaway clusters of scripts/pgtest.sh (SIFT_TESTING= release flavor, or the environment's), for an index
# with positions and one without, each on --rows documents (default 200000: a term in 9 of 10, so terms above the 128K postings of a
# group-sized batch and above what a 1MB budget buffers), at maintenance_work_mem = 1MB, the stages:
#   build   CREATE INDEX with build_segments = 4          merge   sift.merge(.., true) of the four
#   churn   UPDATE of half the rows, promotion, DELETE of a third, VACUUM (that leaves the segments, with dead documents)
#   rewrite sift.merge(.., true) of what is left (rewrites segments with dead documents)
# After each, the file of the index (the first 10 bytes of a page, its LSN and checksum, apart) of the other builds against the
# first. Prints one line a stage and build; exit 1 when a page differs.
# Ports SIFT_PORT (default 56150) and up. The clusters and their directories are removed at the end.
set -Eeuo pipefail
trap 'echo "merge-bytes: failed at line $LINENO: $BASH_COMMAND" >&2' ERR
export LC_ALL=C

REPO=$(cd "$(dirname "$0")/.." && pwd)
ROWS=200000 BUILDS=()
while (( $# )); do
	case $1 in
		--rows) ROWS=$2; shift ;;
		--build) BUILDS+=("$2"); shift ;;
		*) sed -n '2,19p' "$0"; exit 1 ;;
	esac
	shift
done
(( ${#BUILDS[@]} > 1 )) || { sed -n '2,19p' "$0"; exit 1; }
BASE=$(mktemp -d /tmp/sift-merge-bytes.XXXXXX)
PORT0=${SIFT_PORT:-56150}
declare -A DIR TMP PORT

pg() { # LABEL pgtest-args...
	local l=$1; shift
	( cd "${DIR[$l]}" && SIFT_TMP=${TMP[$l]} SIFT_PORT=${PORT[$l]} SIFT_TESTING=${SIFT_TESTING-} scripts/pgtest.sh "$@" )
}
q() { # LABEL sql...: one psql session, one -c per statement
	local l=$1 a=() s; shift
	for s in "$@"; do a+=(-c "$s"); done
	pg "$l" psql -q -At -v ON_ERROR_STOP=1 "${a[@]}"
}
cleanup() {
	for l in "${!DIR[@]}"; do pg "$l" destroy >/dev/null 2>&1 || true; done
	rm -rf "$BASE"
}
trap cleanup EXIT

# The stages of a table t$1 with an index options $2
stage_build() { q "$1" "SET maintenance_work_mem = '1MB'; SET max_parallel_maintenance_workers = 0; CREATE INDEX $2_idx ON $2 USING sift (body) WITH (build_segments = 4$3)"; }
stage_merge() { q "$1" "SET maintenance_work_mem = '1MB'" "SELECT sift.merge('$2_idx', true)"; }
stage_churn() {
	q "$1" "SET maintenance_work_mem = '1MB'; SET sift.async_maintenance = off" "UPDATE $2 SET body = body WHERE id % 2 = 0" \
		"SELECT sift.promote('$2_idx')" "DELETE FROM $2 WHERE id % 3 = 0" "ALTER INDEX $2_idx SET (vacuum_segments = 128)" "VACUUM $2"
}
STAGES=(build merge churn rewrite)
stage_rewrite() { stage_merge "$@"; }

file_of() { echo "${TMP[$1]}/data/$(q "$1" "CHECKPOINT; SELECT pg_relation_filepath('$2_idx')" | tail -1)"; }
differs() { # FILE FILE: the pages that differ, but for the LSN and the checksum
	python3 -I -c '
import sys
a = open(sys.argv[1], "rb").read(); b = open(sys.argv[2], "rb").read()
bad = [i // 8192 for i in range(0, min(len(a), len(b)), 8192) if a[i + 10:i + 8192] != b[i + 10:i + 8192]]
print("%d vs %d bytes, %d pages differ%s" % (len(a), len(b), len(bad), " (first: %d)" % bad[0] if bad else ""))
sys.exit(1 if bad or len(a) != len(b) else 0)' "$1" "$2"
}

i=0
for b in "${BUILDS[@]}"; do
	l=${b%%=*}
	DIR[$l]=$(cd "${b#*=}" && pwd) TMP[$l]=$BASE/$l PORT[$l]=$((PORT0 + i++))
	mkdir -p "${TMP[$l]}"
	pg "$l" install >/dev/null
	pg "$l" start >/dev/null
	q "$l" "ALTER SYSTEM SET autovacuum = off" "SELECT pg_reload_conf()" "CREATE EXTENSION pg_sift" >/dev/null
	q "$l" "SELECT setseed(0.42)" "CREATE TABLE tp (id serial, body text)" "CREATE TABLE tn (id serial, body text)" \
		"INSERT INTO tp (body) SELECT (CASE WHEN i % 10 < 9 THEN 'hot ' ELSE '' END) || (SELECT string_agg('w' || floor(power(random(), 3) * 5000)::int, ' ') FROM generate_series(1, 12) w WHERE i > 0) FROM generate_series(1, $ROWS) i" \
		"INSERT INTO tn SELECT * FROM tp" >/dev/null
done

rc=0
for t in "tp:" "tn:, positions = off"; do
	for s in "${STAGES[@]}"; do
		for b in "${BUILDS[@]}"; do "stage_$s" "${b%%=*}" "${t%%:*}" "${t#*:}" >/dev/null; done
		for b in "${BUILDS[@]:1}"; do
			printf '%-3s %-8s %-5s ' "${t%%:*}" "$s" "${b%%=*}"
			differs "$(file_of "${BUILDS[0]%%=*}" "${t%%:*}")" "$(file_of "${b%%=*}" "${t%%:*}")" || rc=1
		done
	done
done
exit $rc
