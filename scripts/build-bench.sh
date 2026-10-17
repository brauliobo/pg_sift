#!/usr/bin/env bash
# CREATE INDEX benchmark of one or more builds of sift, in throwaway clusters of scripts/pgtest.sh.
#
#   scripts/build-bench.sh [--rows N] [--mem "1MB 16MB 64MB"] [--workers "0 2"] [--runs N] [--build LABEL=DIR]...
#
# Every --build is a checkout built (release flavor) and measured, e.g. --build old=/tmp/1.3.0 --build new=.
# (default: new=this checkout). The builds are interleaved, --runs times per cell (default 3). Prints one table:
#   seconds   mean and standard error of CREATE INDEX
#   peak MB   most anonymous memory the cluster's processes took during the build, above what they took before
#   temp MB   bytes of temporary files (the runs)       runs  runs merged into segments (DEBUG2 log of the build)
#   segs      segments of the index                     index MB  size of the index
# Articles: scripts/build-bench.sql (default 200000 documents of 300 words). A failed build shows its error.
# Ports SIFT_PORT (default 56150) and up. The clusters and their directories are removed at the end.
set -euo pipefail
export LC_ALL=C

REPO=$(cd "$(dirname "$0")/.." && pwd)
ROWS=200000 MEM="1MB 16MB 64MB" WORKERS="0 2" RUNS=3 BUILDS=()
while (( $# )); do
	case $1 in
		--rows) ROWS=$2; shift ;;
		--mem) MEM=$2; shift ;;
		--workers) WORKERS=$2; shift ;;
		--runs) RUNS=$2; shift ;;
		--build) BUILDS+=("$2"); shift ;;
		*) sed -n '2,15p' "$0"; exit 1 ;;
	esac
	shift
done
(( ${#BUILDS[@]} )) || BUILDS=("new=$REPO")
BASE=$(mktemp -d /tmp/sift-build-bench.XXXXXX)
PORT0=${SIFT_PORT:-56150}
declare -A DIR TMP PORT

pg() { # LABEL psql-args...: the build's cluster
	local l=$1; shift
	( cd "${DIR[$l]}" && SIFT_TMP=${TMP[$l]} SIFT_PORT=${PORT[$l]} SIFT_TESTING= scripts/pgtest.sh "$@" )
}
cleanup() {
	for l in "${!DIR[@]}"; do pg "$l" destroy >/dev/null 2>&1 || true; done
	rm -rf "$BASE"
}
trap cleanup EXIT

anon_kb() { # anonymous memory of the processes of a cluster (children of its postmaster)
	local pm
	pm=$(head -1 "$1/data/postmaster.pid")
	for p in $(pgrep -P "$pm"); do awk '/^RssAnon/ {print $2}' "/proc/$p/status" 2>/dev/null; done | awk '{s += $1} END {print s + 0}'
}

peak_kb() { # LABEL: polls until the file $BASE/stop exists, prints the highest sample
	local best=0 kb
	while [[ ! -e $BASE/stop ]]; do
		kb=$(anon_kb "${TMP[$1]}")
		(( kb > best )) && best=$kb
		sleep 0.1
	done
	echo "$best"
}

cell() { # LABEL MEM WORKERS: one build; prints "seconds peakMB tempMB runs segs indexMB" or "FAIL message"
	local l=$1 mem=$2 w=$3 base t0 t1 out err
	rm -f "$BASE/stop"
	base=$(anon_kb "${TMP[$l]}")
	peak_kb "$l" >"$BASE/peak" &
	local poller=$!
	t0=$(date +%s.%N)
	if ! err=$(pg "$l" psql -q -At -v ON_ERROR_STOP=1 -f /dev/stdin 2>&1 <<-EOF
		SELECT temp_bytes AS tb0 FROM pg_stat_database WHERE datname = current_database() \gset
		SET maintenance_work_mem = '$mem';
		SET max_parallel_maintenance_workers = $w;
		SET client_min_messages = debug2;
		CREATE INDEX art_idx ON art USING sift (body);
		RESET client_min_messages;
		SELECT pg_stat_force_next_flush();
		SELECT temp_bytes - :tb0 AS temp_bytes FROM pg_stat_database WHERE datname = current_database() \gset
		\\echo RESULT :temp_bytes
		SELECT 'SEGS ' || count(*) FROM sift.segments('art_idx');
		SELECT 'SIZE ' || pg_relation_size('art_idx');
		DROP INDEX art_idx;
		EOF
	); then
		touch "$BASE/stop"; wait "$poller"
		echo "FAIL $(grep -m1 -E "ERROR|rror" <<<"$err" | cut -c1-80)"
		pg "$l" psql -q -c "DROP INDEX IF EXISTS art_idx" >/dev/null
		return
	fi
	t1=$(date +%s.%N)
	touch "$BASE/stop"; wait "$poller"
	out=$err
	printf '%.2f %d %d %d %d %d\n' "$(bc <<<"$t1 - $t0")" $(( ($(<"$BASE/peak") - base) / 1024 )) \
		$(( $(sed -n 's/^RESULT //p' <<<"$out") / 1048576 )) \
		"$(sed -n 's/.*merging \([0-9]*\) runs into segments.*/\1/p' <<<"$out" | awk '{s += $1} END {print s + 0}')" \
		"$(sed -n 's/^SEGS //p' <<<"$out")" $(( $(sed -n 's/^SIZE //p' <<<"$out") / 1048576 ))
}

i=0
for b in "${BUILDS[@]}"; do
	l=${b%%=*}
	DIR[$l]=$(cd "${b#*=}" && pwd) TMP[$l]=$BASE/$l PORT[$l]=$((PORT0 + i++))
	mkdir -p "${TMP[$l]}"
	pg "$l" install >/dev/null
	pg "$l" start >/dev/null
	pg "$l" psql -q -v rows="$ROWS" -f "$REPO/scripts/build-bench.sql" >/dev/null
done
echo "load average $(cut -d' ' -f1-3 /proc/loadavg), $ROWS articles, $RUNS builds per row"
printf '%-6s %-5s %3s  %-14s %7s %7s %5s %5s %8s\n' build mem wrk seconds 'peak MB' 'temp MB' runs segs 'index MB'
for mem in $MEM; do
	for w in $WORKERS; do
		declare -A res=()
		for r in $(seq "$RUNS"); do
			for b in "${BUILDS[@]}"; do res[${b%%=*}]+="$(cell "${b%%=*}" "$mem" "$w")"$'\n'; done
		done
		for b in "${BUILDS[@]}"; do
			l=${b%%=*}
			if grep -q FAIL <<<"${res[$l]}"; then
				printf '%-6s %-5s %3s  %s\n' "$l" "$mem" "$w" "$(grep -m1 FAIL <<<"${res[$l]}")"
				continue
			fi
			awk -v l="$l" -v m="$mem" -v w="$w" 'NF {n++; s += $1; q += $1 * $1; for (i = 2; i <= 6; i++) last[i] = $i; if ($2 > pk) pk = $2}
				END {mean = s / n; se = n > 1 ? sqrt((q / n - mean * mean) * n / (n - 1) / n) : 0
				printf "%-6s %-5s %3s  %6.2f +- %-5.2f %7d %7d %5d %5d %8d\n", l, m, w, mean, se, pk, last[3], last[4], last[5], last[6]}' <<<"${res[$l]}"
		done
	done
done
