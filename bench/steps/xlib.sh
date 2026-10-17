# Sourced by bench/steps/articles.sh and rank_check.sh: the glue of the engines outside Postgres (adapters with EXTERNAL set:
# Elasticsearch over HTTP, Tantivy in process), driven by the Rust load generator bench/xbench over the same documents, query
# trace, client count and CPU/RAM budget as the Postgres engines. HTTP engines are driven over keep-alive connections from outside
# the budget, so their latency includes HTTP and JSON; Tantivy runs inside it. The requests are rendered by the adapter's RENDER SQL in
# the engine's database (`e_<engine>`, a clone of `bench` with sql/harness.sql: the `docs`, the workloads, the query trace).
# The adapters define the same function names (x_start, x_stop, x_build, x_size, x_pid), so every call goes through `xe`.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

X=$DATA/$TIER
xe() { ( load_engine "$1"; shift; "$@" ); }   # run a function of the engine's adapter in a subshell
is_external() { ( load_engine "$1"; [[ -n $EXTERNAL ]] ); }

# --- the functions below run for the engine loaded by xe, with $RUN its scratch directory
x_render() { { echo "$RENDER"; echo "$1"; } | PGDATABASE=$(engine_db "$ENGINE") psql -X -v ON_ERROR_STOP=1 -qAtF $'\t'; }

x_export_docs() {
  [[ -s $X/docs.ndjson ]] && return 0
  mkdir -p "$X"
  PGDATABASE=bench q -v FETCH_COUNT=20000 -c "SELECT json_build_object('id', id::text, 'body', body, 'created', created - date '1970-01-01', 'category_id', category_id)
                                              FROM docs ORDER BY id" >"$X/docs.ndjson.tmp"
  mv "$X/docs.ndjson.tmp" "$X/docs.ndjson"
}

# $1 class [$2 the class whose queries it renders] -> $RUN/<class>.q (`qid <TAB> request` lines); fails when the engine cannot
# render one of the queries (the join needs the Postgres table)
x_class_file() {
  local wl=${2:-$1} cutoff kind
  [[ $wl != join ]] || return 1
  cutoff=$(PGDATABASE=bench q -c "SELECT v::date - date '1970-01-01' FROM params WHERE k = 'recent_cutoff'")
  kind=$(PGDATABASE=$(engine_db "$ENGINE") q -c "SELECT kind FROM workloads WHERE wl = '$wl'")
  x_render "SELECT c.qid, pg_temp.x_req('$kind', c.mode, c.terms, $cutoff) FROM class_queries c WHERE c.wl = '$wl' ORDER BY c.qid, c.mode" >"$RUN/$1.q"
  ! grep -qP '^\d+\t$' "$RUN/$1.q"
}

# the writes of the update class, UPDATE_RATE per second for the whole cell (SECS, WARM of the caller): 70% update, 15% insert, 15% delete
x_writes_file() {
  # shellcheck disable=SC2153  # SECS and WARM are set by steps/articles.sh
  x_render "WITH m AS (SELECT max(id) AS n FROM docs),
            g AS (SELECT g, m.n, abs(hashint8(g)) % 100 AS r, 1 + abs(hashint8(g * 3 + 1)) % m.n AS dst,
                         1 + abs(hashint8(g * 7 + 2)) % m.n AS src
                  FROM m, generate_series(1, $(( UPDATE_RATE * (SECS + WARM) * 11 / 10 ))) g)
            SELECT pg_temp.x_write(CASE WHEN r < 70 THEN 'upd' WHEN r < 85 THEN 'ins' ELSE 'del' END,
                     jsonb_build_object('id', (CASE WHEN r BETWEEN 70 AND 84 THEN n + g ELSE dst END)::text, 'body', d.body,
                                        'created', d.created - date '1970-01-01', 'category_id', d.category_id))
            FROM g JOIN docs d ON d.id = g.src ORDER BY g.g" >"$RUN/writes.tsv"
  ! grep -q '^$' "$RUN/writes.tsv"
}

x_xb() {
  if [[ $EXTERNAL == tantivy ]]; then limited "$XBENCH" tantivy "$URL" "$@"
  else loadgen "$XBENCH" http "$URL" "$@" "${XB_ARGS[@]}"; fi
}

x_wchar() { [[ -n $1 ]] && grep -oP '^wchar: \K[0-9]+' "/proc/$1/io" || echo 0; }
x_field() { grep -oP "^$1 .*\\b$2=\\K[0-9.]+" "$3"; }

# prepare the engine for the classes of $CLASSES: its database, the documents, a server, the index; builds.tsv row
x_prepare() {
  local t0 c
  clone_db "$(engine_db "$ENGINE")" >/dev/null
  x_export_docs
  x_start
  t0=$(now_ms); locked x_build "$X/docs.ndjson"
  printf '%s\t%s\t%s\t0\t%.1f\t%s\t0\n' "$TIER" "$ENGINE" "$(wc -l <"$X/docs.ndjson")" "$(( $(now_ms) - t0 ))e-3" "$(x_size)" >"$RUN/builds.row"
  log "$ENGINE built in $(( $(now_ms) - t0 )) ms, size $(x_size)"
  for c in $CLASSES; do
    rm -f "$RUN/$c.q"
    if [[ $c == update ]]; then { x_class_file update mixed && x_writes_file; } || rm -f "$RUN/update.q"
    else x_class_file "$c" || rm -f "$RUN/$c.q"; fi
  done
}

# $1 class, $2 seconds, $3 warm (no writers): queries per second of the clients, 0 when the run failed; the update class leaves
# $RUN/update.wps (writer operations per second) and $RUN/update.wal (bytes written by the engine process)
x_cell() {
  local c=$1 secs=$2 extra=() pid w0 out=$RUN/$1.out
  [[ $c == update && -z ${3:-} ]] && extra=(--writes "$RUN/writes.tsv" --write-rate "$UPDATE_RATE")
  pid=$(x_pid); w0=$(x_wchar "$pid")
  x_xb "$RUN/$c.q" --clients "$CLIENTS" --warmup 0 --duration "$secs" "${extra[@]}" >"$out" 2>"$RUN/$c.err" || { echo 0; return; }
  if [[ ${extra[*]:-} ]]; then
    x_field write qps "$out" >"$RUN/update.wps" || echo 0 >"$RUN/update.wps"
    grep -oP '^io wchar=\K[0-9]+' "$out" >"$RUN/update.wal" || echo $(( $(x_wchar "$pid") - w0 )) >"$RUN/update.wal"
  fi
  x_field read qps "$out" || echo 0
}

# the first $1 trace substrings as and / or / phrase top-10s: `qid <TAB> {ids}` lines (the format of rank_check.sh)
x_top10() {
  x_render "SELECT q.id, pg_temp.x_req('top10', q.mode, q.terms, 0) FROM queries q WHERE q.sub <= $1 ORDER BY q.id" >"$RUN/rank.q"
  ! grep -qP '^\d+\t$' "$RUN/rank.q" || return 1
  x_xb "$RUN/rank.q" --ids-out "$RUN/rank.ids" >/dev/null && cat "$RUN/rank.ids"
}
