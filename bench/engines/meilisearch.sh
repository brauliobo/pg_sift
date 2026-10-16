# Meilisearch (bench/bench setup --engines meilisearch), typo tolerance off. Not BM25: ranking rules
# (words, proximity, exactness...). `or` uses matchingStrategy=last (drops trailing words until
# something matches), which is not a true disjunction. Latency comparison only; no exact counts.
EXTERNAL=http
MEILI_BIN=$ROOT/engines/meilisearch/dl/meilisearch-linux-amd64
MEILI_DIR=$ROOT/engines/meilisearch/$TIER
URL=http://127.0.0.1:56060
XB_ARGS=(--took /processingTimeMs --ids-ptr /hits --id-ptr /id)
REQUIRES_PATH=$MEILI_BIN

x_start() {
  mkdir -p "$MEILI_DIR"
  (cd "$MEILI_DIR" && limited "$MEILI_BIN" --db-path "$MEILI_DIR/db" --http-addr 127.0.0.1:56060 --no-analytics \
    --env development --max-indexing-threads 4 --max-indexing-memory 16GiB >"$MEILI_DIR/log" 2>&1 &)
  until curl -fs "$URL/health" >/dev/null; do sleep 1; done
}
x_pid() { pgrep -f -- "--db-path $MEILI_DIR/db"; }
x_stop() { kill "$(x_pid)"; while x_pid >/dev/null; do sleep 1; done; rm -rf "$MEILI_DIR"; }
meili() { curl -fsS -H "Content-Type: ${4:-application/json}" -X "$1" "$URL$2" --data-binary "$3" | grep -oP '"taskUid":\K[0-9]+'; }
meili_wait() { until curl -fsS "$URL/tasks/$1" | grep -qE '"status":"(succeeded|failed)"'; do sleep 2; done; curl -fsS "$URL/tasks/$1" | grep -q succeeded; }

x_build() {
  meili_wait "$(meili POST /indexes '{"uid": "docs", "primaryKey": "id"}')"
  meili_wait "$(meili PATCH /indexes/docs/settings '{"searchableAttributes": ["body"], "filterableAttributes": ["created"],
    "displayedAttributes": ["id"], "typoTolerance": {"enabled": false}, "pagination": {"maxTotalHits": 1000}}')"
  local last
  split -l 500000 "$1" "$MEILI_DIR/p."
  for p in "$MEILI_DIR"/p.*; do last=$(meili POST /indexes/docs/documents "@$p" application/x-ndjson); rm "$p"; done
  meili_wait "$last"
}
x_size() { curl -fsS "$URL/stats" | grep -oP '"databaseSize":\K[0-9]+'; }

RENDER=$(cat <<'SQL'
CREATE FUNCTION pg_temp.x_req(kind text, mode text, terms text[], cutoff int) RETURNS text LANGUAGE sql AS $$
  SELECT '/indexes/docs/search' || E'\t' || (jsonb_build_object('limit', 10, 'attributesToRetrieve', jsonb_build_array('id'),
      'q', CASE mode WHEN 'phrase' THEN '"' || array_to_string(terms, ' ') || '"' ELSE array_to_string(terms, ' ') END,
      'matchingStrategy', CASE mode WHEN 'or' THEN 'last' ELSE 'all' END)
    || CASE kind WHEN 'filter' THEN jsonb_build_object('filter', 'created >= ' || cutoff) ELSE '{}' END)::text
  WHERE kind IN ('top10', 'filter')
$$;
CREATE FUNCTION pg_temp.x_write(op text, doc jsonb) RETURNS text LANGUAGE sql AS $$ SELECT NULL::text $$;
SQL
)
