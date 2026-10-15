# Elasticsearch single node (bench/fetch.sh elasticsearch), 1 shard, 0 replicas, standard analyzer
# (lowercase, no stemming, no stopwords). _source is disabled so the store size is index-only, like
# a Postgres index size. Shard request cache is off so repeated count queries are not cached.
EXTERNAL=http
ES_HOME=$ROOT/engines/elasticsearch/es
ES_DIR=$ROOT/engines/elasticsearch/$TIER
URL=http://127.0.0.1:56050
XB_ARGS=(--took /took --ids-ptr /hits/hits --id-ptr /_id --count-ptr /hits/total/value)
REQUIRES_PATH=$ES_HOME/bin/elasticsearch

x_start() {
  mkdir -p "$ES_DIR"
  ES_JAVA_OPTS="-Xms${ES_HEAP:-4g} -Xmx${ES_HEAP:-4g} -XX:-AlwaysPreTouch" limited "$ES_HOME/bin/elasticsearch" -d -p "$ES_DIR/pid" \
    -E path.data="$ES_DIR/data" -E path.logs="$ES_DIR/logs" -E http.port=56050 -E transport.port=56051 \
    -E network.host=127.0.0.1 -E discovery.type=single-node -E node.processors=8 \
    -E xpack.security.enabled=false -E xpack.ml.enabled=false -E cluster.routing.allocation.disk.threshold_enabled=false \
    >"$ES_DIR/start.log"
  until curl -fs "$URL/_cluster/health?wait_for_status=yellow" >/dev/null; do sleep 1; done
}
x_stop() { kill "$(cat "$ES_DIR/pid")"; while [[ -e $ES_DIR/pid ]]; do sleep 1; done; rm -rf "$ES_DIR"; }
x_pid() { cat "$ES_DIR/pid"; }
es() { curl -fsS -H 'Content-Type: application/json' -X "$1" "$URL$2" ${3:+--data-binary "$3"} >/dev/null; }

# stdin: NDJSON docs whose first key is "id" (field 4 when split on quotes).
es_bulk() {
  awk -F'"' '{ print "{\"index\":{\"_id\":\"" $4 "\"}}"; print }' |
    curl -fsS -H 'Content-Type: application/x-ndjson' -X POST "$URL/docs/_bulk" --data-binary @- | grep -q '"errors":false'
}
bulk_part() { export -f es_bulk; export URL; SHELL=/bin/bash split -l 5000 --filter=es_bulk "$1"; }

x_build() {
  es PUT /docs '{"settings": {"number_of_shards": 1, "number_of_replicas": 0, "refresh_interval": "-1",
                              "index.translog.durability": "async"},
                 "mappings": {"dynamic": false, "_source": {"enabled": false},
                              "properties": {"body": {"type": "text"}, "created": {"type": "integer"}, "category_id": {"type": "integer"}}}}'
  local tmp=$ES_DIR/parts; mkdir -p "$tmp"
  split -n l/4 "$1" "$tmp/p."
  for p in "$tmp"/p.*; do bulk_part "$p" & done; wait
  rm -rf "$tmp"
  es POST /docs/_refresh
  es POST '/docs/_forcemerge?max_num_segments=1'
  es PUT /docs/_settings '{"index": {"refresh_interval": "1s"}}'
}
x_size() { curl -fsS "$URL/docs/_stats/store" | grep -m1 -oP '"primaries":\{"store":\{"size_in_bytes":\K[0-9]+' | head -1; }

RENDER=$(cat <<'SQL'
CREATE FUNCTION pg_temp.es_q(mode text, terms text[]) RETURNS jsonb LANGUAGE sql AS $$
  SELECT CASE mode
    WHEN 'and'    THEN jsonb_build_object('match', jsonb_build_object('body', jsonb_build_object('query', array_to_string(terms, ' '), 'operator', 'and')))
    WHEN 'or'     THEN jsonb_build_object('match', jsonb_build_object('body', jsonb_build_object('query', array_to_string(terms, ' '), 'operator', 'or')))
    WHEN 'phrase' THEN jsonb_build_object('match_phrase', jsonb_build_object('body', array_to_string(terms, ' ')))
    WHEN 'prefix' THEN jsonb_build_object('prefix', jsonb_build_object('body', terms[1]))
    WHEN 'fuzzy'  THEN jsonb_build_object('fuzzy', jsonb_build_object('body', jsonb_build_object('value', terms[1], 'fuzziness', 1)))
  END
$$;
CREATE FUNCTION pg_temp.x_req(kind text, mode text, terms text[], cutoff int) RETURNS text LANGUAGE sql AS $$
  SELECT '/docs/_search?request_cache=false' || E'\t' || CASE kind
    WHEN 'top10'  THEN jsonb_build_object('size', 10, '_source', false, 'track_total_hits', false, 'query', pg_temp.es_q(mode, terms))
    WHEN 'count'  THEN jsonb_build_object('size', 0, 'track_total_hits', true, 'query', pg_temp.es_q(mode, terms))
    WHEN 'filter' THEN jsonb_build_object('size', 10, '_source', false, 'track_total_hits', false, 'query', jsonb_build_object('bool',
                         jsonb_build_object('must', pg_temp.es_q(mode, terms), 'filter', jsonb_build_object('range', jsonb_build_object('created', jsonb_build_object('gte', cutoff))))))
  END::text
$$;
CREATE FUNCTION pg_temp.x_write(op text, doc jsonb) RETURNS text LANGUAGE sql AS $$
  SELECT CASE op WHEN 'del' THEN 'DELETE' ELSE 'PUT' END || E'\t/docs/_doc/' || (doc->>'id')
         || CASE op WHEN 'del' THEN '' ELSE E'\t' || doc::text END
$$;
SQL
)
