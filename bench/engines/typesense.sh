# Typesense (bench/fetch.sh typesense), in-memory index, typos and prefix matching off. Not BM25
# (text-match score + proximity). `or` relies on drop_tokens_threshold, which drops query words
# until enough hits exist: not a true disjunction. Latency comparison only; no exact counts.
EXTERNAL=http
TS_BIN=$ROOT/engines/typesense/dl/typesense-server
TS_DIR=$ROOT/engines/typesense/$TIER
TS_KEY=sift-bench
URL=http://127.0.0.1:56070
XB_ARGS=(--took /results/0/search_time_ms --ids-ptr /results/0/hits --id-ptr /document/id --header "X-TYPESENSE-API-KEY: $TS_KEY")
REQUIRES_PATH=$TS_BIN

x_start() {
  mkdir -p "$TS_DIR/data"
  limited "$TS_BIN" --data-dir "$TS_DIR/data" --api-key "$TS_KEY" --api-address 127.0.0.1 --api-port 56070 \
    --peering-address 127.0.0.1 --peering-port 56071 --thread-pool-size 16 >"$TS_DIR/log" 2>&1 &
  until curl -fs "$URL/health" | grep -q true; do sleep 1; done
}
x_pid() { pgrep -f -- "--data-dir $TS_DIR/data"; }
x_stop() { kill "$(x_pid)"; while x_pid >/dev/null; do sleep 1; done; rm -rf "$TS_DIR"; }
ts() { curl -fsS -H "X-TYPESENSE-API-KEY: $TS_KEY" -X "$1" "$URL$2" --data-binary "$3"; }

x_build() {
  ts POST /collections '{"name": "docs", "fields": [{"name": "body", "type": "string"},
    {"name": "created", "type": "int32", "optional": true}, {"name": "prop_id", "type": "int32", "optional": true}]}' >/dev/null
  split -l 100000 "$1" "$TS_DIR/p."
  for p in "$TS_DIR"/p.*; do
    if ts POST '/collections/docs/documents/import?action=create&batch_size=10000' "@$p" | grep -m3 '"success":false' >&2; then return 1; fi
    rm "$p"
  done
}
x_size() { grep -oP 'VmRSS:\s+\K[0-9]+' "/proc/$(x_pid)/status" | awk '{print $1 * 1024}'; }

RENDER=$(cat <<'SQL'
CREATE FUNCTION pg_temp.x_req(kind text, mode text, terms text[], cutoff int) RETURNS text LANGUAGE sql AS $$
  SELECT '/multi_search' || E'\t' || jsonb_build_object('searches', jsonb_build_array(
      jsonb_build_object('collection', 'docs', 'query_by', 'body', 'per_page', 10, 'include_fields', 'id',
        'num_typos', 0, 'prefix', false, 'exhaustive_search', false,
        'q', CASE mode WHEN 'phrase' THEN '"' || array_to_string(terms, ' ') || '"' ELSE array_to_string(terms, ' ') END,
        'drop_tokens_threshold', CASE mode WHEN 'or' THEN 10 ELSE 0 END)
      || CASE kind WHEN 'filter' THEN jsonb_build_object('filter_by', 'created:>=' || cutoff) ELSE '{}' END))::text
  WHERE kind IN ('top10', 'filter')
$$;
CREATE FUNCTION pg_temp.x_write(op text, doc jsonb) RETURNS text LANGUAGE sql AS $$ SELECT NULL::text $$;
SQL
)
