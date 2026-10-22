#!/usr/bin/env bash
# Randomized differential test of sift against the SQL reference
# (test/sql/reference.sql) and seq-scan sift.match; harness in test/diff/diff.sql.
#
#   scripts/difftest.sh [--seed N] [--docs N] [--vocab N] [--queries N] [--rounds N]
#                       [--k N] [--engine sift|mutant:NAME|mutants] [--mutants "NAME..."] [--minimize N] [--min-trials N]
#                       [--jobs N]
#   scripts/difftest.sh --repro FILE
#   INDEX_OPTS="WITH (positions = off)" scripts/difftest.sh ...   build the index with options
#
# Each round checks every query in every stage (parse, seqscan, bitmap, count, ranked and the parallel
# scan_par, count_par, ranked_par) plus verify/stats/tokenize, then mutates the corpus (deletes, updates,
# inserts) with VACUUM, promotion and merge interleaved; the last round ends with REINDEX so ranked
# scores are compared bit for bit. Stages whose sift code has not landed report
# "skip". The first failure per stage and round (at most --minimize per round)
# is minimized (query, then corpus) into $SIFT_TMP/difftest/*.sql, replayable
# with --repro. --engine mutants runs the harness
# against the reference with injected bugs (all of MUTANTS, or the --mutants
# subset) and fails if any bug survives. --jobs N (DIFFTEST_JOBS, default 1) checks the queries of a round in N sessions
# (query i in session i mod N), and the mutants N at a time; each query seeds its own random() (filters of the ranked
# checks), so a run does not depend on N. The parallel stages ask for 3 workers each: keep 3 * N within max_parallel_workers.
# Needs the scripts/pgtest.sh cluster installed and started. Exit 1 on failure.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-arch}
OUT=$SIFT_TMP/difftest
SEED=1 DOCS=5000 VOCAB=3000 QUERIES=100 ROUNDS=3 K=10 ENGINE=sift REPRO= MIN_TRIALS=40 MAXMIN=3 JOBS=${DIFFTEST_JOBS:-1}
MUTANTS="not_ignored phrase_as_and float8_score idf_df_plus1 tie_ctid_desc boost_ignored dismax_sum topk_short
	count_plus nofold_query tf_cap15 prefix_trunc fuzzy_no_firstchar drop_high_offset drop_sb_boundary avgdl_int"

while (( $# )); do
	case $1 in
		--seed) SEED=$2 ;; --docs) DOCS=$2 ;; --vocab) VOCAB=$2 ;; --queries) QUERIES=$2 ;;
		--rounds) ROUNDS=$2 ;; --k) K=$2 ;; --engine) ENGINE=$2 ;; --repro) REPRO=$2 ;;
		--min-trials) MIN_TRIALS=$2 ;; --minimize) MAXMIN=$2 ;; --mutants) MUTANTS=$2 ;; --jobs) JOBS=$2 ;;
		*) sed -n '2,21p' "$0"; exit 1 ;;
	esac
	shift 2
done

sql() { PGOPTIONS='-c client_min_messages=warning' SIFT_DB=${DB:-sift_diff} "$REPO/scripts/pgtest.sh" psql -v ON_ERROR_STOP=1 -qAt "$@"; }
fseed() { awk -v x="$1" 'BEGIN { printf "%.6f", ((x * 7919) % 19999) / 10000 - 1 }'; }
log() { printf '[difftest %s] %s\n' "$(date +%T)" "$*" >&2; }

fresh_db() {
	DB=postgres sql -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB"
	sql -c "CREATE EXTENSION pg_sift"
	sql -v sift_ref_lib=1 -f "$REPO/test/sql/reference.sql" -f "$REPO/test/diff/diff.sql"
	sql -c "CREATE TABLE diff_results (engine text, round int, qno int, q text, stage text, status text, detail text)"
}

# Worker $5 of $6: the queries of a round with qno mod $6 = $5, and (worker 0) the checks of the round itself. Every query
# seeds random() with its own number (the generated queries come from the round's seed, the filters of the ranked checks
# from the query's), so the run does not depend on the number of workers.
check_worker() {
	sql -v round="$1" -v clean="$2" -v idx="$3" -v engine="$4" -v worker="$5" -v workers="$6" -v k="$K" \
		-v seed="$(fseed $((SEED * 1000 + $1)))" -v nq="$QUERIES" -v rseed="$((SEED * 1000 + $1))" <<-'SQL'
		SELECT set_config('sift_diff.idx', :'idx', false), set_config('sift_diff.engine', :'engine', false) \gset
		SELECT :worker = 0 AS first \gset
		SET statement_timeout = '300s';
		SELECT sift_ref.load('diff_docs', 'body') \gset
		CREATE TEMP TABLE q AS SELECT row_number() OVER () AS qno, q FROM sift_diff.gen_queries(:seed, :nq) q;
		\set ON_ERROR_STOP 0
		SELECT format('SELECT setseed(hashint8(%s::bigint * 1000003 + %s)::float8 / 2147483648.0);
			INSERT INTO diff_results SELECT %L, %s, %s, %L, c.* FROM sift_diff.check(%L, %s, %L) c',
			:rseed, qno, :'engine', :round, qno, q, q, :k, :clean) FROM q WHERE qno % :workers = :worker ORDER BY qno \gexec
		\if :first
			INSERT INTO diff_results SELECT :'engine', :round, 0, NULL, c.*
			FROM sift_diff.check_round(:clean, array(SELECT q FROM q ORDER BY qno)) c;
		\endif
		INSERT INTO diff_results SELECT :'engine', :round, qno, q, 'harness', 'fail', 'no result: crash, timeout or harness error'
		FROM q WHERE qno % :workers = :worker AND qno NOT IN (SELECT qno FROM diff_results WHERE engine = :'engine' AND round = :round);
	SQL
}

# spawn CMD...: in the background, at most JOBS at a time; drain waits for all and fails if one failed
bad=0
spawn() {
	while (( $(jobs -rp | wc -l) >= JOBS )); do wait -n || bad=1; done
	"$@" &
}
drain() {
	local p
	for p in $(jobs -p); do wait "$p" || bad=1; done
	(( ! bad )) || { log "a worker failed"; exit 1; }
}

# Runs one round's checks ($5 workers, default JOBS), then minimizes its first failure per stage
check_round() {
	local round=$1 clean=$2 idx=$3 engine=$4 w workers=${5:-$JOBS}
	for (( w = 0; w < workers; w++ )); do spawn check_worker "$round" "$clean" "$idx" "$engine" "$w" "$workers"; done
	drain
	local n=0
	while IFS=$'\t' read -r qno stage q; do
		(( n++ < MAXMIN )) || break
		local file=$OUT/fail-$engine-s$SEED-r$round-q$qno-$stage.sql
		log "minimizing round $round query $qno ($stage): $q"
		sql -v idx="$idx" -v engine="$engine" -v q="$q" -v stage="$stage" -v k="$K" -v clean="$clean" \
			-v trials="$MIN_TRIALS" >"$file" <<-'SQL' || true
			SELECT set_config('sift_diff.idx', :'idx', false), set_config('sift_diff.engine', :'engine', false) \gset
			SELECT sift_ref.load('diff_docs', 'body') \gset
			SELECT sift_diff.minimize_all(:'q', :'stage', :k, :clean, :trials);
		SQL
		log "  -> $file: $(sed -n 's/^-- minimized query: //p' "$file")"
	done < <(sql -F $'\t' -v engine="$engine" -v round="$round" <<<"SELECT DISTINCT ON (stage) qno, stage, q FROM diff_results
		WHERE engine = :'engine' AND round = :round AND status = 'fail' AND qno > 0 ORDER BY stage, qno")
}

run_engine() {
	local engine=$1 idx=diff_idx clean=true
	sql -c "SELECT sift_diff.build($(fseed "$SEED"), $DOCS, $VOCAB)"
	if [[ $engine != sift ]]; then
		idx=
	elif ! err=$(sql -c "CREATE INDEX diff_idx ON diff_docs USING sift (body) ${INDEX_OPTS:-}" 2>&1); then
		[[ $err == *"not implemented"* ]] || sql -c "INSERT INTO diff_results VALUES ('$engine', 0, 0, NULL, 'build', 'fail', \$\$$err\$\$)"
		log "CREATE INDEX: ${err#*ERROR:  }"
		idx=
	fi
	for (( round = 0; round <= ROUNDS; round++ )); do
		if (( round > 0 )); then
			log "round $round: $(sql -c "SELECT sift_diff.mutate($(fseed $((SEED * 7 + round))), 0.05)")"
			clean=false
			(( round % 2 == 0 )) && sql -c "VACUUM diff_docs"
			# odd rounds are checked with the mutations in write buffers, even ones after promotion (and merge)
			[[ -n $idx ]] && (( round % 2 == 0 )) && sql -c "SELECT sift.promote('$idx')"
			[[ -n $idx ]] && (( round % 4 == 0 )) && sql -c "SELECT sift.merge('$idx', true)"
			if (( round == ROUNDS )); then
				sql -c "VACUUM diff_docs"
				[[ -n $idx ]] && sql -c "REINDEX INDEX diff_idx"
				clean=true
			fi
		fi
		log "round $round ($engine, clean=$clean): checking $QUERIES queries"
		check_round "$round" "$clean" "$idx" "$engine"
	done
}

summary() {
	sql -P footer=off -P format=aligned -c "SELECT engine, stage, count(*) FILTER (WHERE status = 'ok') AS ok,
		count(*) FILTER (WHERE status = 'fail') AS fail, count(*) FILTER (WHERE status = 'skip') AS skip,
		count(*) FILTER (WHERE status = 'warn') AS warn FROM diff_results GROUP BY 1, 2 ORDER BY 1, 2" \
		-c "SELECT engine, round, qno, stage, status, left(q, 80) AS query, left(detail, 160) AS detail FROM diff_results
		WHERE status IN ('fail', 'warn') ORDER BY engine, round, qno LIMIT 30"
}

mkdir -p "$OUT"
if [[ -n $REPRO ]]; then
	DB=sift_diff_repro fresh_db
	DB=sift_diff_repro sql -P footer=off -P format=aligned -f "$REPRO"
	exit
fi

DB=sift_diff fresh_db
if [[ $ENGINE == mutants ]]; then
	# all rounds without a bug (no false positives), then each bug on the final corpus
	run_engine "mutant:"
	for m in $MUTANTS; do
		log "mutant $m"
		spawn check_round "$ROUNDS" true "" "mutant:$m" 1
	done
	drain
	summary
	sql -P footer=off -P format=aligned -v mutants="$MUTANTS" <<-'SQL' | tee "$OUT/mutants.txt"
		SELECT m AS mutant, count(DISTINCT (round, qno)) FILTER (WHERE status = 'fail') AS failing_queries,
			string_agg(DISTINCT stage, ',') FILTER (WHERE status = 'fail') AS stages,
			CASE WHEN m = '' THEN CASE WHEN count(*) FILTER (WHERE status = 'fail') = 0 THEN 'clean' ELSE 'FALSE POSITIVE' END
				WHEN count(*) FILTER (WHERE status = 'fail') > 0 THEN 'killed' ELSE 'SURVIVED' END AS verdict
		FROM unnest(string_to_array(regexp_replace(' ' || :'mutants', '\s+', ' ', 'g'), ' ')) m
		LEFT JOIN diff_results r ON r.engine = 'mutant:' || m GROUP BY m ORDER BY m;
	SQL
	! grep -qE 'SURVIVED|FALSE POSITIVE' "$OUT/mutants.txt"
	exit
fi

run_engine "$ENGINE"
summary | tee "$OUT/summary-s$SEED.txt"
[[ $(sql -c "SELECT count(*) FROM diff_results WHERE status = 'fail'") == 0 ]]
