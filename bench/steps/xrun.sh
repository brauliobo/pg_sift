#!/usr/bin/env bash
# Usage: TIER=Q|F bench/steps/xrun.sh ENGINE [WORKLOAD...]
#   ENGINE: elasticsearch tantivy meilisearch typesense (bench/engines/*.sh with EXTERNAL set)
#   WORKLOAD: and or phrase mixed count filter join update rank (default: all; join is Postgres-only,
#   rank = top-10 overlap with the pgx-bm25 reference from rank_check.sh, untimed), or `quick`: the
#   standard suite (sql/quick.sql), REPEAT (50) requests per client and class after 5 warm-up ones
# Same documents, query trace, client count and CPU/RAM budget as run.sh. HTTP engines are driven by
# bench/xbench over keep-alive connections from outside the budget; latency is client-observed and
# includes HTTP + JSON; the engine-reported time is in the note (took).
. "$(dirname "$0")/lib.sh"

ENGINE=${1:?engine}; shift
WORKLOADS=${*:-and or phrase mixed count filter join update rank}
DURATION=${DURATION:-30} CLIENTS=${CLIENTS:-8} WARMUP=${WARMUP:-5} UPDATE_RATE=${UPDATE_RATE:-1000} REPEAT=${REPEAT:-50}
XBENCH=$ROOT/target/release/xbench
load_engine "$ENGINE"
[[ -x $REQUIRES_PATH ]] || { log "$ENGINE: $REQUIRES_PATH missing (bench/steps/fetch.sh), skipping"; exit 0; }
RUN=$OUT/$ENGINE X=$DATA/$TIER
mkdir -p "$RUN" "$X"
"$STEPS/cluster.sh" start

declare -A KIND=([and]=top10 [or]=top10 [phrase]=top10 [mixed]=top10 [count]=count [filter]=filter [rank]=top10)
# shellcheck disable=SC2054  # comma separated lists are the values
declare -A MODES=([and]=and [or]=or [phrase]=phrase [mixed]=and,or,phrase [count]=and,or,phrase [filter]=and,or,phrase [rank]=and,or,phrase)

render() { { echo "$RENDER"; echo "$1"; } | psql -X -v ON_ERROR_STOP=1 -qAtF $'\t'; }

export_docs() {
  [[ -s $X/docs.ndjson ]] && return
  q -v FETCH_COUNT=20000 -c "SELECT json_build_object('id', id::text, 'body', body, 'created', created - date '1970-01-01', 'category_id', category_id)
                             FROM docs ORDER BY id" >"$X/docs.ndjson.tmp"
  mv "$X/docs.ndjson.tmp" "$X/docs.ndjson"
}

# Writes `qid \t request` lines; fails when the engine cannot render one of the workload's modes.
wl_file() {
  local cutoff sub=${2:-1000000}
  if [[ $1 == q_* ]]; then
    render "$(cat "$BENCH/sql/quick.sql") SELECT q.sub, pg_temp.x_req(q.kind, q.mode, q.terms, 0) FROM quick q
            WHERE q.class = '${1#q_}' ORDER BY q.sub, q.mode" >"$RUN/$1.q"
    ! grep -qP '^\d+\t$' "$RUN/$1.q"
    return
  fi
  cutoff=$(q -c "SELECT v::date - date '1970-01-01' FROM params WHERE k = 'recent_cutoff'")
  render "SELECT q.id, pg_temp.x_req('${KIND[$1]}', q.mode, q.terms, $cutoff) FROM queries q
          WHERE q.mode = ANY ('{${MODES[$1]}}') AND q.sub <= $sub ORDER BY q.id" >"$RUN/$1.q"
  ! grep -qP '^\d+\t$' "$RUN/$1.q"
}

writes_file() {
  render "WITH m AS (SELECT max(id) AS n FROM docs),
          g AS (SELECT g, m.n, abs(hashint8(g)) % 100 AS r, 1 + abs(hashint8(g * 3 + 1)) % m.n AS dst,
                       1 + abs(hashint8(g * 7 + 2)) % m.n AS src
                FROM m, generate_series(1, $(( UPDATE_RATE * (DURATION + WARMUP) * 11 / 10 ))) g)
          SELECT pg_temp.x_write(CASE WHEN r < 70 THEN 'upd' WHEN r < 85 THEN 'ins' ELSE 'del' END,
                   jsonb_build_object('id', (CASE WHEN r BETWEEN 70 AND 84 THEN n + g ELSE dst END)::text, 'body', d.body,
                                      'created', d.created - date '1970-01-01', 'category_id', d.category_id))
          FROM g JOIN docs d ON d.id = g.src ORDER BY g.g" >"$RUN/writes.tsv"
  ! grep -q '^$' "$RUN/writes.tsv"
}

xb() {
  if [[ $EXTERNAL == tantivy ]]; then limited "$XBENCH" tantivy "$URL" "$@"
  else "$XBENCH" http "$URL" "$@" "${XB_ARGS[@]}"; fi
}

wchar() { [[ -n $1 ]] && grep -oP '^wchar: \K[0-9]+' "/proc/$1/io" || echo 0; }

build_timed() { local t0; t0=$(now_ms); x_build "$X/docs.ndjson"; echo $(( $(now_ms) - t0 )) >"$RUN/build.ms"; }

timed_run() {
  local wl=$1 qf=$2 extra=() pid w0
  [[ $wl == update ]] && extra=(--writes "$RUN/writes.tsv" --write-rate "$UPDATE_RATE")
  pid=$(x_pid); w0=$(wchar "$pid")
  local span=(--warmup "$WARMUP" --duration "$DURATION")
  [[ $wl == q_* ]] && span=(--warmup 5 --repeat "$REPEAT")
  xb "$RUN/$qf.q" --clients "$CLIENTS" "${span[@]}" --log "$RUN/$wl.lat" "${extra[@]}" \
    >"$RUN/$wl.out" 2>"$RUN/$wl.err"
  grep -oP '^io wchar=\K[0-9]+' "$RUN/$wl.out" >"$RUN/$wl.wal" || echo $(( $(wchar "$pid") - w0 )) >"$RUN/$wl.wal"
}

field() { grep -oP "^$1 .*\\b$2=\\K[0-9.]+" "$RUN/$3.out"; }

row() { printf '%s\t' "$TIER" "$ENGINE" "$@" | sed 's/\t$/\n/' >>"$OUT/results.tsv"; }

measure() {
  local wl=$1 qf=$1 note
  [[ $wl == update ]] && qf=mixed
  if [[ $wl == join ]] || ! wl_file "$qf" || { [[ $wl == update ]] && ! writes_file; }; then
    row "$wl" "" "" "" "" "" "" "" "" "" "" "" "unsupported"; log "$ENGINE $wl: unsupported"; return
  fi
  locked timed_run "$wl" "$qf"
  note="took_avg=$(field read took_avg "$wl") took_p99=$(field read took_p99 "$wl") errors=$(field read errors "$wl")"
  [[ $wl == update ]] && note="reads=mixed writes=$(field write n "$wl") writer_tps=$(field write qps "$wl") writer_p99=$(field write p99 "$wl") $note"
  row "$wl" "$CLIENTS" "$DURATION" "$(field read qps "$wl")" "$(field read avg "$wl")" "$(field read p50 "$wl")" "$(field read p95 "$wl")" \
    "$(field read p99 "$wl")" "$(field read n "$wl")" "" "$(cat "$RUN/$wl.wal")" "$(loadavg)" "$note"
  log "$ENGINE $wl: $(grep "^read" "$RUN/$wl.out")"
}

rank() {
  local ref=$DATA/rankref-$TIER-200.tsv
  [[ -s $ref ]] || { log "rank: run bench/steps/rank_check.sh first to create $ref"; return; }
  wl_file rank 200 || return
  xb "$RUN/rank.q" --ids-out "$RUN/rank.ids"
  q -c "CREATE TEMP TABLE got (id int, ids bigint[]); CREATE TEMP TABLE ref (id int, ids bigint[])" \
    -c "\\copy got FROM '$RUN/rank.ids'" -c "\\copy ref FROM '$ref'" -F $'\t' \
    -c "SELECT '$ENGINE', q.mode, count(*), round(avg(cardinality(ARRAY(SELECT unnest(g.ids) INTERSECT SELECT unnest(r.ids)))::numeric
               / nullif(cardinality(r.ids), 0)), 3) FROM got g JOIN ref r USING (id) JOIN queries q USING (id) GROUP BY 2 ORDER BY 2" |
    tee "$OUT/rank-$ENGINE.txt"
}

export_docs
x_start
trap x_stop EXIT
locked build_timed
printf '%s\t%s\t%s\t0\t%.1f\t%s\t0\n' "$TIER" "$ENGINE" "$(wc -l <"$X/docs.ndjson")" "$(cat "$RUN/build.ms")e-3" "$(x_size)" >>"$OUT/builds.tsv"
log "$ENGINE built in $(cat "$RUN/build.ms") ms, size $(x_size)"
[[ " $WORKLOADS " == *" quick "* ]] && WORKLOADS=${WORKLOADS/quick/$(psql -X -qAt -f "$BENCH/sql/quick.sql" -c "SELECT DISTINCT 'q_' || class FROM quick ORDER BY 1" | tr '\n' ' ')}
for wl in $WORKLOADS; do
  if [[ $wl == rank ]]; then rank; else measure "$wl"; fi
done
