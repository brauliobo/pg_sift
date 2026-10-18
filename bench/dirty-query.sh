#!/usr/bin/env bash
# What dead documents cost a query, and how far they move the scores, against the dead fraction of the index.
#
#   bench/dirty-query.sh [-n DOCS] [-T SECONDS] [-f 'FRACTION...'] [-m delete|rollback|oldest] [-o FILE] HOST:PORT[/DATABASE]
#
# For each fraction (default 0 0.1 0.25 0.45) a table of DOCS documents (default 200000) is built with the same seed, then:
#   delete    a random fraction of the rows is deleted and VACUUMed: the index keeps the postings of the dead documents in
#             segments with dirty superblocks (liveness bits cleared, statistics unchanged) until a merge rewrites them
#   oldest    the same with the first rows by id: the dirty superblocks are contiguous (and, being the half of the documents
#             that repeat w7 three times, they move the statistics of w7)
#   rollback  documents worth that fraction of the live ones, all repeating w7, are loaded and rolled back, then the index is
#             promoted and merged into one segment: postings of dead heap tuples that VACUUM has not marked yet (the mutable
#             segments that hold them before the promotion, and the segments it makes, are costs of their own)
# Each query is timed (pgbench, SECONDS per query, default 2; count_par with two parallel workers) and its top 10 kept; the index is then rebuilt (REINDEX) on the same
# live rows and the queries are timed and compared again. A row is: the mode, the dead fraction, the dead documents as a percentage
# of the index's, the query, milliseconds dirty and clean, their ratio, how many of the clean top 10 the dirty one returned, and the dirty top score
# over the clean one. -o appends the rows to FILE as TSV. HOST:PORT is as in bench/ab-query.sh; the database is created if missing.
set -euo pipefail
export LC_ALL=C PGOPTIONS='-c client_min_messages=warning'

docs=200000 secs=2 fracs='0 0.1 0.25 0.45' mode=delete out=
while getopts n:T:f:m:o: o; do
	case $o in n) docs=$OPTARG ;; T) secs=$OPTARG ;; f) fracs=$OPTARG ;; m) mode=$OPTARG ;; o) out=$OPTARG ;; *) sed -n '2,19p' "$0"; exit 1 ;; esac
done
shift $((OPTIND - 1))
(( $# == 1 )) && [[ $mode == delete || $mode == oldest || $mode == rollback ]] || { sed -n '2,19p' "$0"; exit 1; }
host=${1%%:*} rest=${1#*:}
port=${rest%%/*} db=bench
[[ $rest == */* ]] && db=${rest#*/}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

psql_() { psql -X -q -At -v ON_ERROR_STOP=1 -h "$host" -p "$port" -U postgres "$@"; }
psql -X -q -At -h "$host" -p "$port" -U postgres postgres <<SQL
SELECT 'CREATE DATABASE $db' WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = '$db')
\gexec
SQL

# name|statement; the ranked ones return the top 10 and are the ones compared
queries=(
	'and2|SELECT ctid FROM dq WHERE body @@> '"'w5 w50'"' ORDER BY sift.score(ctid) DESC LIMIT 10'
	'or3|SELECT ctid FROM dq WHERE body @@> '"'w5 OR w50 OR w7'"' ORDER BY sift.score(ctid) DESC LIMIT 10'
	'rare|SELECT ctid FROM dq WHERE body @@> '"'w1500 OR w1501 OR w20'"' ORDER BY sift.score(ctid) DESC LIMIT 10'
	'count|SELECT count(*) FROM dq WHERE body @@> '"'w5'"
	'count_and|SELECT count(*) FROM dq WHERE body @@> '"'w5 w50'"
	'count_or|SELECT count(*) FROM dq WHERE body @@> '"'w5 OR w50 OR w7'"
	'count_par|SELECT count(*) FROM dq WHERE body @@> '"'w5'"
	'scan|SELECT count(ctid) FROM dq WHERE body @@> '"'w300 OR w301'"' AND id % 7 <> 0'
)

time_ms() { # SQL NAME -> milliseconds per statement; the *_par ones run with parallel workers
	local opts=$PGOPTIONS
	[[ $2 != *_par ]] || opts+=' -c max_parallel_workers_per_gather=2 -c parallel_setup_cost=0 -c parallel_tuple_cost=0 -c min_parallel_index_scan_size=0 -c sift.parallel_scan=on'
	printf '%s;\n' "$1" >"$work/q.sql"
	PGOPTIONS=$opts pgbench -n -f "$work/q.sql" -M prepared -t 300 -h "$host" -p "$port" -U postgres "$db" >/dev/null
	PGOPTIONS=$opts pgbench -n -f "$work/q.sql" -M prepared -T "$secs" -h "$host" -p "$port" -U postgres "$db" | sed -n 's/^latency average = \([0-9.]*\) ms.*/\1/p'
}

top10() { # SQL -> "ids|top score" of the ranked statement, from the table of this run
	psql_ "$db" -c "SELECT coalesce(string_agg(id::text, ',' ORDER BY s DESC, id), ''), coalesce(max(s), 0) FROM
		(SELECT id, sift.score(ctid) s FROM dq WHERE body @@> '$1' ORDER BY sift.score(ctid) DESC, id LIMIT 10) t"
}

build() { # fraction
	psql_ "$db" -v docs="$docs" -v f="$1" -v mode="$mode" <<-'SQL'
		CREATE EXTENSION IF NOT EXISTS pg_sift;
		DROP TABLE IF EXISTS dq;
		CREATE TABLE dq (id serial PRIMARY KEY, body text) WITH (autovacuum_enabled = off, parallel_workers = 2);
		CREATE OR REPLACE FUNCTION dq_doc(skew bool) RETURNS text LANGUAGE sql AS
			$$ SELECT (CASE WHEN skew THEN 'w7 w7 w7 ' ELSE '' END) ||
				(SELECT string_agg('w' || floor(3000 * power(random(), 3))::int, ' ') FROM generate_series(1, 30)) $$;
		CREATE OR REPLACE FUNCTION dq_rollback(n int) RETURNS void LANGUAGE plpgsql AS $$
			BEGIN
				INSERT INTO dq (body) SELECT dq_doc(true) FROM generate_series(1, n);
				RAISE EXCEPTION 'rollback';
			EXCEPTION WHEN raise_exception THEN NULL;
			END $$;
		SELECT setseed(0.42);
		INSERT INTO dq (body) SELECT dq_doc(i <= :docs / 2) FROM generate_series(1, :docs) i;
		CREATE INDEX dq_idx ON dq USING sift (body);
		VACUUM ANALYZE dq;
		SELECT (:'mode' = 'delete') AS del, (:'mode' = 'oldest') AS old, (:'mode' = 'rollback') AS rb \gset
		\if :del
			DELETE FROM dq WHERE random() < :f;
		\elif :old
			DELETE FROM dq WHERE id <= (:f * :docs)::int;
		\elif :rb
			SELECT dq_rollback((:f * :docs)::int) \g /dev/null
			SELECT sift.promote('dq_idx'), sift.merge('dq_idx', true) \g /dev/null
		\endif
		\if :rb
		\else
			VACUUM dq;
		\endif
	SQL
}

dirty_share() { # documents of the index whose row is gone, as a percentage of the index's
	psql_ "$db" -c "SELECT round(100.0 * (sum(ndocs) - (SELECT count(*) FROM dq)) / nullif(sum(ndocs), 0), 1) FROM sift.segments('dq_idx')"
}

for f in $fracs; do
	build "$f"
	dead=$(dirty_share)
	declare -A dms=() dtop=()
	for q in "${queries[@]}"; do
		n=${q%%|*} s=${q#*|}
		dms[$n]=$(time_ms "$s" "$n")
		[[ $s == *"LIMIT 10"* ]] && dtop[$n]=$(top10 "$(sed -n "s/.*@@> '\([^']*\)'.*/\1/p" <<<"$s")")
	done
	psql_ "$db" -c "REINDEX INDEX dq_idx" -c "VACUUM dq"
	for q in "${queries[@]}"; do
		n=${q%%|*} s=${q#*|}
		cms=$(time_ms "$s" "$n")
		same=- ratio=-
		if [[ -n ${dtop[$n]:-} ]]; then
			ct=$(top10 "$(sed -n "s/.*@@> '\([^']*\)'.*/\1/p" <<<"$s")")
			ci=${ct%|*} di=${dtop[$n]%|*}
			same=$(comm -12 <(tr , '\n' <<<"$ci" | sort) <(tr , '\n' <<<"$di" | sort) | grep -c . || true)
			ratio=$(awk -v d="${dtop[$n]#*|}" -v c="${ct#*|}" 'BEGIN { printf "%.3f", (c > 0) ? d / c : 0 }')
		fi
		row=$(awk -v m="$mode" -v f="$f" -v dead="$dead" -v n="$n" -v d="${dms[$n]}" -v c="$cms" -v same="$same" -v r="$ratio" \
			'BEGIN { printf "%s\t%s\t%s\t%s\t%.3f\t%.3f\t%.2f\t%s\t%s\n", m, f, dead, n, d, c, d / c, same, r }')
		printf '%s\n' "$row"
		[[ -z $out ]] || printf '%s\n' "$row" >>"$out"
	done
	unset dms dtop
done
psql_ "$db" -c "DROP TABLE IF EXISTS dq" -c "DROP FUNCTION IF EXISTS dq_doc(bool), dq_rollback(int)"
