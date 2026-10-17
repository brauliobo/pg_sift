# Sourced by bench/steps/articles.sh, places.sh and rank_check.sh: the glue of the engines outside Postgres (adapters with EXTERNAL set:
# Elasticsearch over HTTP, Tantivy in process), driven by the Rust load generator bench/xbench over the same documents, query
# trace and client count as the Postgres engines. HTTP engines are driven over keep-alive connections, so their latency includes HTTP
# and JSON; Tantivy runs in the load generator's process. The requests are rendered by
# the adapter's RENDER SQL (articles) and PLACES_RENDER (places) from the Postgres tables, the same trace the Postgres engines run.
# The cells of an HTTP engine are measured by steps/cell.py like the pgbench ones (it stops at the same relative standard error);
# Tantivy keeps a fixed time per cell. The adapters define the same function names (x_start, x_stop, x_build, x_size, x_pid), so every
# call goes through `xe`.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

X=$DATA/$TIER
xe() { ( load_engine "$1"; shift; "$@" ); }   # run a function of the engine's adapter in a subshell
is_external() { ( load_engine "$1"; [[ -n $EXTERNAL ]] ); }
is_http() { ( load_engine "$1"; [[ $EXTERNAL == http ]] ); }

# --- the functions below run for the engine loaded by xe, with $RUN its scratch directory
x_render() { { echo "$RENDER"; echo "$1"; } | PGDATABASE=$(engine_db "$ENGINE") psql -X -v ON_ERROR_STOP=1 -qAtF $'\t'; }
xp_render() { { echo "$PLACES_RENDER"; echo "$1"; } | PGDATABASE=places psql -X -v ON_ERROR_STOP=1 -qAtF $'\t'; }

# the documents (articles with the region of their category, the place fields) as NDJSON, once per tier and size
x_export_docs() {
  local f=$X/articles-${TIER_DOCS[$TIER]}.ndjson
  [[ -s $f ]] && { echo "$f"; return 0; }
  mkdir -p "$X"
  PGDATABASE=$(engine_db "$ENGINE") q -v FETCH_COUNT=20000 -c "SELECT json_build_object('id', d.id::text, 'body', d.body, 'created', d.created - date '1970-01-01', 'category_id', d.category_id, 'region', c.region)
                                              FROM docs d LEFT JOIN categories c ON c.id = d.category_id ORDER BY d.id" >"$f.tmp"
  mv "$f.tmp" "$f"
  echo "$f"
}
xp_export_docs() { # places.ndjson of the tier's places
  local f; f=$X/places-$(basename "$DATADIR").ndjson
  [[ -s $f ]] && { echo "$f"; return 0; }
  mkdir -p "$X"
  PGDATABASE=places q -v FETCH_COUNT=20000 -c "SELECT json_build_object('id', id::text, 'name', $PLACES_NAME, 'text', $PLACES_TEXT, 'country_code', country_code, 'population', population)
                                               FROM places ORDER BY id" >"$f.tmp"
  mv "$f.tmp" "$f"
  echo "$f"
}

# $1 class [$2 the class whose queries it renders] -> $RUN/<class>.q (`qid <TAB> request` lines in the order of the statements the Postgres
# engines run, so that the first requests are the cold-cache ones); fails when the engine cannot render one of the queries
x_class_file() {
  local wl=${2:-$1} cutoff kind
  [[ $wl != join || -n $JOINS ]] || return 1
  cutoff=$(PGDATABASE=$(engine_db "$ENGINE") q -c "SELECT v::date - date '1970-01-01' FROM params WHERE k = 'recent_cutoff'")
  kind=$(PGDATABASE=$(engine_db "$ENGINE") q -c "SELECT kind FROM workloads WHERE wl = '$wl'")
  x_render "SELECT c.qid, pg_temp.x_req('$kind', c.mode, c.terms, $cutoff) FROM class_queries c WHERE c.wl = '$wl' ORDER BY hashtext(c.qid::text || c.mode), c.qid, c.mode" >"$RUN/$1.q"
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
                                        'created', d.created - date '1970-01-01', 'category_id', d.category_id, 'region', c.region))
            FROM g JOIN docs d ON d.id = g.src LEFT JOIN categories c ON c.id = d.category_id ORDER BY g.g" >"$RUN/writes.tsv"
  ! grep -q '^$' "$RUN/writes.tsv"
}

x_xb() {
  if [[ $EXTERNAL == tantivy ]]; then "$XBENCH" tantivy "$URL" "$@"
  else "$XBENCH" http "$URL" "$@" "${XB_ARGS[@]}"; fi
}

x_wchar() { [[ -n $1 ]] && grep -oP '^wchar: \K[0-9]+' "/proc/$1/io" || echo 0; }
x_field() { grep -oP "^$1 .*\\b$2=\\K[0-9.]+" "$3"; }

# prepare the engine for the classes of $CLASSES: its database, the documents, a server, the index; builds.row (the bulk load and refresh of
# an engine that merges afterwards, with the merge as its own row)
x_builds() { # INDEX ELAPSED_MS: `variant TAB seconds TAB bytes` lines of the build: the bulk load and refresh (the elapsed time when the engine
  # reports none), and, for an engine that merges afterwards, the force-merge as its own line with the size before it (`build.ms` of the adapter)
  local build=$2 merge=0 pre=0
  [[ ! -s $RUN/build.ms ]] || read -r build merge pre <"$RUN/build.ms"
  printf -- '-\t%.1f\t%s\n' "${build}e-3" "$(x_size "$1")"
  (( merge == 0 )) || printf 'forcemerge\t%.1f\t%s\n' "${merge}e-3" "$pre"
}
x_prepare() {
  local t0 c f v s b
  adopt_db "$(engine_db "$ENGINE")" >/dev/null || return 1   # the engine's database holds the documents and the params
  f=$(x_export_docs) || return 1
  x_start || return 1
  rm -f "$RUN/build.ms"
  t0=$(now_ms); locked x_build "$f" || return 1
  x_builds docs $(( $(now_ms) - t0 )) | while IFS=$'\t' read -r v s b; do   # tier engine docs setup_s build_s index_bytes heap_growth variant
    printf '%s\t%s\t%s\t0\t%s\t%s\t0\t%s\n' "$TIER" "$ENGINE" "$(wc -l <"$f")" "$s" "$b" "$v"
  done >"$RUN/builds.row"
  log "$ENGINE built in $(( $(now_ms) - t0 )) ms, size $(x_size)"
  for c in $CLASSES; do
    rm -f "$RUN/$c.q"
    if [[ $c == update ]]; then { x_class_file update mixed && x_writes_file; } || rm -f "$RUN/update.q"
    else x_class_file "$c" || rm -f "$RUN/$c.q"; fi
  done
}

# places: the engine's index of the tier's places and the requests of every workload and class (`$RUN/<workload>.<class>.q`, the first 300 queries
# of the class, the ones the Postgres engines draw from); builds.row in the format of places.sh
xp_prepare() { # WORKLOADS CLASSES, in the engine's lane with the `places` database in place
  local t0 wl c f v s b
  f=$(xp_export_docs) || return 1
  x_start || return 1
  rm -f "$RUN/build.ms"
  t0=$(now_ms); locked x_build_places "$f" || return 1
  x_builds places $(( $(now_ms) - t0 )) | while IFS=$'\t' read -r v s b; do   # engine build_s index_bytes heap_growth setup_s variant
    printf '%s\t%s\t%s\t0\t0\t%s\n' "$ENGINE" "$s" "$b" "$v"
  done >"$RUN/builds.row"
  log "$ENGINE built in $(( $(now_ms) - t0 )) ms, size $(x_size places)"
  for wl in $1; do for c in $2; do
    xp_render "SELECT p.id, pg_temp.xp_req('$wl', p.q) FROM pq p WHERE p.class = '$c' ORDER BY p.id LIMIT ${XP_QUERIES:-300}" >"$RUN/$wl.$c.q" || return 1
  done; done
}

xp_ids() { # WORKLOAD N: the engine's answers to the first N queries of every class of pq (sql/places_oracle.sql): `qid TAB {ids}` lines in $RUN/check.<workload>.ids
  xp_render "SELECT p.id, pg_temp.xp_req('$1', p.q) FROM (SELECT *, row_number() OVER (PARTITION BY class ORDER BY id) AS rn FROM pq) p WHERE p.rn <= $2 ORDER BY p.id" >"$RUN/check.$1.q"
  x_xb "$RUN/check.$1.q" --ids-out "$RUN/check.$1.ids" >/dev/null
  echo "$RUN/check.$1.ids"
}

# --- cells. $1 is the request file; the cell is measured by steps/cell.py (mode qps: 8 clients; latency: 1 client) like a pgbench one
x_cell_se() { # MODE FILE [HINT]: the output of cell.py, queries per second (qps, stderr, n, seconds) or p50, p95, stderr, n, seconds (latency)
  # shellcheck disable=SC2153  # RSE, MIN_TIME, SECS, MIN_N are set by articles.sh / places.sh
  python3 -I "$STEPS/cell.py" "$1" --rse "$RSE" --min-time "$MIN_TIME" --max-time "$SECS" --min-n "$MIN_N" --hint "${3:-$2.hint}" --tmp "$RUN" --xbench -- \
    "$XBENCH" http "$URL" "$2" --clients "$([[ $1 == qps ]] && echo "${CLIENTS:-8}" || echo 1)" "${XB_ARGS[@]}"
}
x_warm() { # SECONDS CLIENTS FILE...: every file for that long, the answers dropped (the JVM and the caches, what pg_prewarm is for Postgres)
  local s=$1 n=$2 f; shift 2
  for f in "$@"; do x_xb "$f" --clients "$n" --warmup 0 --duration "$s" >/dev/null 2>&1 || true; done
}

# $1 class, $2 seconds, $3 warm (no writers): queries per second of the clients, 0 when the run failed; the update class leaves
# $RUN/update.wps (writer operations per second) and $RUN/update.wal (bytes written by the engine process). A class of an HTTP engine
# other than update is measured by cell.py (`qps stderr n seconds`), the others for a fixed time
x_cell() {
  local c=$1 extra=() pid w0 out=$RUN/$1.out
  if [[ $EXTERNAL == http && $c != update ]]; then
    if [[ -n ${3:-} ]]; then x_warm "$2" "${CLIENTS:-8}" "$RUN/$c.q"; else x_cell_se qps "$RUN/$c.q" "$RUN/$c.hint" || printf '0\tNA\t0\t0\n'; fi
    return
  fi
  [[ $c == update && -z ${3:-} ]] && extra=(--writes "$RUN/writes.tsv" --write-rate "$UPDATE_RATE")
  pid=$(x_pid); w0=$(x_wchar "$pid")
  x_xb "$RUN/$c.q" --clients "$CLIENTS" --warmup 0 --duration "$2" "${extra[@]}" >"$out" 2>"$RUN/$c.err" || { echo 0; return; }
  if [[ ${extra[*]:-} ]]; then
    x_field write qps "$out" >"$RUN/update.wps" || echo 0 >"$RUN/update.wps"
    grep -oP '^io wchar=\K[0-9]+' "$out" >"$RUN/update.wal" || echo $(( $(x_wchar "$pid") - w0 )) >"$RUN/update.wal"
  fi
  x_field read qps "$out" || echo 0
}

# --- cold cache. Elasticsearch has no per-relation eviction: before every request the caches of the index are cleared, the index is closed
# and opened again (the shard drops its readers and every mapped segment, the heap structures of the index) and, when the data directory
# is not in memory (tmpfs), the pages of its files are dropped from the OS page cache (posix_fadvise, no root needed). Time: the client's
# (curl's time_total, HTTP included: Elasticsearch reports `took` in whole milliseconds), in ms.
x_evict_files() {
  [[ $(stat -f -c %T "$ES_DIR") == tmpfs ]] && return 0
  python3 -I - "$ES_DIR/data" <<'PY'
import os, sys
for root, _, files in os.walk(sys.argv[1]):
    for name in files:
        fd = os.open(os.path.join(root, name), os.O_RDONLY); os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED); os.close(fd)
PY
}
x_cold_ms() { # FILE I INDEX: the cold time of line I of the request file
  local path body idx=$3
  IFS=$'\t' read -r _ path body < <(sed -n "${2}p" "$1")
  curl -fsS -X POST "$URL/$idx/_cache/clear" >/dev/null
  curl -fsS -X POST "$URL/$idx/_close" >/dev/null
  curl -fsS -X POST "$URL/$idx/_open?wait_for_active_shards=1" >/dev/null
  curl -fsS "$URL/_cluster/health/$idx?wait_for_status=yellow&timeout=60s" >/dev/null
  x_evict_files
  curl -fsS -o /dev/null -w '%{time_total}\n' -H 'Content-Type: application/json' -X POST "$URL$path" --data-binary "$body" | awk '{printf "%.2f\n", $1 * 1000}'
}
x_cold_class() { # FILE INDEX: `p50 TAB max TAB n` of the first NQ requests of the file
  X_COLD_FILE=$1 X_COLD_INDEX=$2
  # shellcheck disable=SC2329 # called by cold_cell
  cold_req() { x_cold_ms "$X_COLD_FILE" "$1" "$X_COLD_INDEX"; }
  cold_cell cold_req
}

# the first $1 trace substrings as and / or / phrase top-10s: `qid <TAB> {ids}` lines (the format of rank_check.sh)
x_top10() {
  x_render "SELECT q.id, pg_temp.x_req('top10', q.mode, q.terms, 0) FROM queries q WHERE q.sub <= $1 ORDER BY q.id" >"$RUN/rank.q"
  ! grep -qP '^\d+\t$' "$RUN/rank.q" || return 1
  x_xb "$RUN/rank.q" --ids-out "$RUN/rank.ids" >/dev/null && cat "$RUN/rank.ids"
}
# the match sets and counts of the oracle's queries (sql/oracle.sql oracle_queries) for the first $1 substrings: `qid TAB {ids}` lines, then
# `qid TAB count` lines in $RUN/oracle.counts
x_oracle_answers() { # NSUB MODES
  x_render "SELECT q.id, pg_temp.x_req('match', q.mode, q.terms, 0) FROM oracle_queries q WHERE q.sub <= $1 AND q.mode = ANY (string_to_array('$2', ' ')) ORDER BY q.id" >"$RUN/oracle.q"
  x_render "SELECT q.id, pg_temp.x_req('count', q.mode, q.terms, 0) FROM oracle_queries q WHERE q.sub <= $1 AND q.mode = ANY (string_to_array('$2', ' ')) ORDER BY q.id" >"$RUN/oracle.count.q"
  ! grep -qP '^\d+\t$' "$RUN/oracle.q" "$RUN/oracle.count.q" || return 1
  x_xb "$RUN/oracle.q" --ids-out "$RUN/oracle.ids" >/dev/null && x_xb "$RUN/oracle.count.q" --ids-out "$RUN/oracle.counts" >/dev/null
}
