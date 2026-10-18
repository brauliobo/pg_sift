#!/usr/bin/env bash
# CREATE INDEX benchmark of one or more builds of sift, in throwaway clusters of scripts/pgtest.sh.
#
#   scripts/build-bench.sh [--rows N] [--mem "1MB 16MB 64MB"] [--workers "0 2"] [--corpus "articles hot unique"] [--runs N]
#                          [--build LABEL=DIR]...
#
# Every --build is a checkout built (production flavor) and measured, e.g. --build old=/tmp/1.3.0 --build new=.
# (default: new=this checkout). The builds are interleaved, --runs times per cell (default 3). A memory of "default" leaves
# maintenance_work_mem as the server has it. Prints one table per corpus:
#   seconds   mean and standard error of CREATE INDEX
#   peak MB   most anonymous memory the cluster's processes took during the build, above what they took before
#   temp MB   bytes of temporary files (the runs)       runs  runs merged into segments (DEBUG2 log of the build)
#   segs      segments of the index                     index MB  size of the index
# Corpora (scripts/build-bench.sql): articles (--rows documents of 300 words), hot (10 x --rows documents of 8 words, one term in
# 9 of 10), unique (10 x --rows documents of 5 words, 2 of them unique to the document: a dictionary of 2 terms a document).
# The same documents for the same --rows, whatever the build. A failed build shows its error.
# Ports SIFT_PORT (default 56150) and up. The clusters and their directories are removed at the end.
set -euo pipefail
export LC_ALL=C

REPO=$(cd "$(dirname "$0")/.." && pwd)
ROWS=200000 MEM="1MB 16MB 64MB" WORKERS="0 2" RUNS=3 CORPORA="articles" BUILDS=()
while (( $# )); do
	case $1 in
		--rows) ROWS=$2; shift ;;
		--mem) MEM=$2; shift ;;
		--workers) WORKERS=$2; shift ;;
		--corpus) CORPORA=$2; shift ;;
		--runs) RUNS=$2; shift ;;
		--build) BUILDS+=("$2"); shift ;;
		*) sed -n '2,15p' "$0"; exit 1 ;;
	esac
	shift
done
(( ${#BUILDS[@]} )) || BUILDS=("new=$REPO")
. "$REPO/scripts/multibuild.sh"
MB_TESTING=
mb_begin sift-build-bench

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

cell() { # LABEL MEM WORKERS TABLE: one build; prints "seconds peakMB tempMB runs segs indexMB" or "FAIL message"
	local l=$1 mem=$2 w=$3 tbl=$4 base t0 t1 out err set_mem="SET maintenance_work_mem = '$2';"
	[[ $mem == default ]] && set_mem=''

	rm -f "$BASE/stop"
	base=$(anon_kb "${TMP[$l]}")
	peak_kb "$l" >"$BASE/peak" &
	local poller=$!
	t0=$(date +%s.%N)
	if ! err=$(pg "$l" psql -q -At -v ON_ERROR_STOP=1 -f /dev/stdin 2>&1 <<-EOF
		SELECT temp_bytes AS tb0 FROM pg_stat_database WHERE datname = current_database() \gset
		$set_mem
		SET max_parallel_maintenance_workers = $w;
		SET client_min_messages = debug2;
		CREATE INDEX ${tbl}_idx ON $tbl USING sift (body);
		RESET client_min_messages;
		SELECT pg_stat_force_next_flush();
		SELECT temp_bytes - :tb0 AS temp_bytes FROM pg_stat_database WHERE datname = current_database() \gset
		\\echo RESULT :temp_bytes
		SELECT 'SEGS ' || count(*) FROM sift.segments('${tbl}_idx');
		SELECT 'SIZE ' || pg_relation_size('${tbl}_idx');
		DROP INDEX ${tbl}_idx;
		EOF
	); then
		touch "$BASE/stop"; wait "$poller"
		echo "FAIL $(grep -m1 -E "ERROR|rror" <<<"$err" | cut -c1-80)"
		pg "$l" psql -q -c "DROP INDEX IF EXISTS ${tbl}_idx" >/dev/null
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

mb_start
for l in "${LABELS[@]}"; do
	pg "$l" psql -q -v rows="$ROWS" -f "$REPO/scripts/build-bench.sql" >/dev/null
done
declare -A TABLE=([articles]=art [hot]=hotc [unique]=uniq)
echo "load average $(cut -d' ' -f1-3 /proc/loadavg), $ROWS rows, $RUNS builds per row"
for corpus in $CORPORA; do
printf '%s\n%-6s %-7s %3s  %-14s %7s %7s %5s %5s %8s\n' "corpus $corpus" build mem wrk seconds 'peak MB' 'temp MB' runs segs 'index MB'
for mem in $MEM; do
	for w in $WORKERS; do
		declare -A res=()
		for r in $(seq "$RUNS"); do
			for b in "${BUILDS[@]}"; do res[${b%%=*}]+="$(cell "${b%%=*}" "$mem" "$w" "${TABLE[$corpus]}")"$'\n'; done
		done
		for b in "${BUILDS[@]}"; do
			l=${b%%=*}
			if grep -q FAIL <<<"${res[$l]}"; then
				printf '%-6s %-7s %3s  %s\n' "$l" "$mem" "$w" "$(grep -m1 FAIL <<<"${res[$l]}")"
				continue
			fi
			awk -v l="$l" -v m="$mem" -v w="$w" 'NF {n++; s += $1; q += $1 * $1; for (i = 2; i <= 6; i++) last[i] = $i; if ($2 > pk) pk = $2}
				END {mean = s / n; se = n > 1 ? sqrt((q / n - mean * mean) * n / (n - 1) / n) : 0
				printf "%-6s %-7s %3s  %6.2f +- %-5.2f %7d %7d %5d %5d %8d\n", l, m, w, mean, se, pk, last[3], last[4], last[5], last[6]}' <<<"${res[$l]}"
		done
	done
done
done
