#!/usr/bin/env bash
# User-space instructions and CPU time per transaction of the backend that runs a statement (or a directory of them), on two servers,
# interleaved in rounds. Unlike bench/ab-query.sh it does not depend on the load of the machine: a backend that runs slowly on a
# loaded host still executes the same instructions.
#
#   bench/cpu-query.sh [-n TRANSACTIONS] [-r ROUNDS] [-M simple|extended|prepared] SQL|FILE|DIR NEW REF
#
# NEW and REF are HOST:PORT[/DATABASE] (default database "bench"); a DIR is every *.sql in it, picked at random by pgbench, in the same order on both sides. Needs perf
# (perf_event_paranoid <= 2: counts of the user's own processes) and servers of the user. perf follows the postmaster's children, so
# the count is the one backend of the pgbench client (its start-up is the same on both sides and small against TRANSACTIONS).
# Prints per side the mean +- standard error over the rounds of instructions and microseconds of CPU per transaction, and the
# ratio NEW/REF of both (below 1 is cheaper).
set -euo pipefail
export LC_ALL=C
. "$(dirname "$0")/steps/perf.sh"

txns=2000 rounds=5 mode=simple
while getopts n:r:M: o; do
	case $o in n) txns=$OPTARG ;; r) rounds=$OPTARG ;; M) mode=$OPTARG ;; *) sed -n '2,12p' "$0"; exit 1 ;; esac
done
shift $((OPTIND - 1))
(( $# == 3 )) || { sed -n '2,12p' "$0"; exit 1; }
sql=$1 sides=("$2" "$3")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [[ -d $sql ]]; then files=("$sql"/*.sql); elif [[ -f $sql ]]; then files=("$sql"); else printf '%s\n' "$sql" >"$work/q.sql"; files=("$work/q.sql"); fi
args=(); for f in "${files[@]}"; do args+=(-f "$f"); done

run() { # SIDE -> "instructions-per-transaction cpu-us-per-transaction"
	local host=${1%%:*} rest=${1#*:} db=bench
	local port=${rest%%/*}
	[[ $rest == */* ]] && db=${rest#*/}
	local pm
	pm=$(head -1 "$(psql -X -At -h "$host" -p "$port" -U postgres -d "$db" -c 'SHOW data_directory')/postmaster.pid")
	read -r _ ins cpu < <(perf_counts "$pm" -- pgbench -n --random-seed=7 "${args[@]}" -M "$mode" -c 1 -j 1 -t "$txns" -h "$host" -p "$port" -U postgres "$db")
	awk -v n="$txns" -v i="$ins" -v c="$cpu" 'BEGIN { printf "%.0f %.2f\n", i / n, c * 1000 / n }'
}

for ((r = 0; r < rounds; r++)); do
	for i in 0 1; do
		s=$(( (i + r) % 2 ))
		printf '%s %s\n' "$s" "$(run "${sides[$s]}")" >>"$work/res"
	done
done
awk '
	{ n[$1]++; a[$1] += $2; aq[$1] += $2 * $2; c[$1] += $3; cq[$1] += $3 * $3 }
	function se(s, q, k) { return k > 1 ? sqrt((q - s * s / k) / (k - 1) / k) : 0 }
	END {
		for (s = 0; s < 2; s++)
			printf "%s: %.0f +- %.0f instructions, %.1f +- %.1f us CPU per transaction (%d rounds)\n", s ? "REF" : "NEW",
				a[s] / n[s], se(a[s], aq[s], n[s]), c[s] / n[s], se(c[s], cq[s], n[s]), n[s]
		printf "NEW/REF: instructions %.3f, CPU time %.3f (below 1 is cheaper)\n", (a[0] / n[0]) / (a[1] / n[1]), (c[0] / n[0]) / (c[1] / n[1])
	}' "$work/res"
