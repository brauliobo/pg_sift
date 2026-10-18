#!/usr/bin/env bash
# A/B timing of one statement on two servers (the checkout and a reference build), interleaved in rounds so that load drifts hit both.
#
#   bench/ab-query.sh [-T SECONDS] [-r ROUNDS] [-c CLIENTS] [-M simple|extended|prepared] [-s 'SET ...'] SQL|FILE NEW REF
#
# NEW and REF are HOST:PORT[/DATABASE] (a socket directory or host name, default database "bench"). SQL is a statement, or a file for
# pgbench -f; -s is a statement run before it in every transaction (a SET). -M simple measures planning and execution, prepared
# mostly execution. Prints transactions per second per side as mean +- standard
# error over the rounds and the ratio NEW/REF (above 1 is faster).
set -euo pipefail

secs=3 rounds=7 clients=1 mode=simple pre=
while getopts T:r:c:M:s: o; do
	case $o in T) secs=$OPTARG ;; r) rounds=$OPTARG ;; c) clients=$OPTARG ;; M) mode=$OPTARG ;; s) pre=$OPTARG ;; *) sed -n '2,9p' "$0"; exit 1 ;; esac
done
shift $((OPTIND - 1))
(( $# == 3 )) || { sed -n '2,9p' "$0"; exit 1; }
sql=$1 sides=("$2" "$3")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [[ -f $sql ]]; then cp "$sql" "$work/q.sql"; else printf '%s\n' "$sql" >"$work/q.sql"; fi
[[ -z $pre ]] || { printf '%s;\n' "$pre" | cat - "$work/q.sql" >"$work/q2.sql"; mv "$work/q2.sql" "$work/q.sql"; }

run() { # SIDE -> tps
	local host=${1%%:*} rest=${1#*:}
	local port=${rest%%/*} db=bench
	[[ $rest == */* ]] && db=${rest#*/}
	pgbench -n -f "$work/q.sql" -M "$mode" -c "$clients" -j "$clients" -T "$secs" -h "$host" -p "$port" -U postgres "$db" |
		sed -n 's/^tps = \([0-9.]*\).*/\1/p'
}

for ((r = 0; r < rounds; r++)); do
	# alternate which side goes first
	for i in 0 1; do
		s=$(( (i + r) % 2 ))
		printf '%s %s\n' "$s" "$(run "${sides[$s]}")" >>"$work/tps"
	done
done
awk '
	{ n[$1]++; s[$1] += $2; q[$1] += $2 * $2 }
	END {
		for (i = 0; i < 2; i++) { m[i] = s[i] / n[i]; v = (q[i] - n[i] * m[i] * m[i]) / (n[i] - 1); e[i] = sqrt(v / n[i]) }
		printf "new %.1f +- %.1f tps, ref %.1f +- %.1f tps, new/ref %.3f +- %.3f\n", m[0], e[0], m[1], e[1], m[0] / m[1],
			m[0] / m[1] * sqrt((e[0] / m[0]) ^ 2 + (e[1] / m[1]) ^ 2)
	}' "$work/tps"
