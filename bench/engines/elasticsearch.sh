# Elasticsearch single node, driven over HTTP by bench/xbench. It is the system package (`pacman -S elasticsearch`: /usr/share/elasticsearch,
# its bundled JDK link), started as an ordinary process of the user, not as the package's service, with a configuration directory of its own
# under the scratch root (the one in /etc is not touched, nor readable): nothing is installed or written outside $SIFT_BENCH_ROOT.
# One shard, no replica, security and ML off, a heap as large as the shared_buffers of the Postgres engines (the index itself lives in the
# page cache, outside the heap), the same CPU list and cgroup scope as a Postgres server (`limited`, node.processors = its CPUs). Both
# datasets are loaded through the bulk API, 4 requests at a time, with the documents and ids of the Postgres tables (xlib.sh), then
# refreshed and force-merged to one segment (the build time is the bulk load plus the refresh, the force-merge is its own row and is not
# counted in it; the index size is the size after the merge). The shard request cache and the query (filter) cache are off, so no engine
# answers from a cache of results. The packaged version is recorded in the machine description (steps/machine.sh).
#
# Analysis, the same for every field: tokens are the runs of letters and digits (the pattern tokenizer on [^\p{L}\p{N}]+, the split into
# alphanumeric runs of sift and of the oracle), lowercased and folded to ASCII, no stemming, no stopwords. (The char_group tokenizer
# on whitespace, punctuation and symbols was tried first and left a token out of about one document in 500 at indexing time, with a
# correct _analyze of the same text: the oracle check found it, the pattern tokenizer matches the oracle.) BM25 with k1 = 1.2
# and b = 0.75 set explicitly (Lucene's idf and a one byte quantized length norm); discount_overlaps is off so that the length of a field
# counts every token, the n-grams of the places included (by default the n-grams of a word are overlapping tokens, which have no length).
EXTERNAL=http
JOINS=yes
ES_HOME=${ES_HOME:-/usr/share/elasticsearch}
ES_DIR=$ROOT/engines/elasticsearch/$TIER
ES_PORT=${ES_PORT:-$(( ${TIER_PORT[$TIER]} + 300 ))}
URL=http://127.0.0.1:$ES_PORT
XB_ARGS=(--took /took --ids-ptr /hits/hits --id-ptr /_id --count-ptr /count)
REQUIRES_PATH=$ES_HOME/bin/elasticsearch

ES_ANALYSIS='"similarity": {"bm25": {"type": "BM25", "k1": 1.2, "b": 0.75, "discount_overlaps": false}},
  "analysis": {"tokenizer": {"words": {"type": "pattern", "pattern": "[^\\p{L}\\p{N}]+", "group": -1}},
               "filter": {"edge": {"type": "edge_ngram", "min_gram": 1, "max_gram": 15}},
               "analyzer": {"words": {"type": "custom", "tokenizer": "words", "filter": ["lowercase", "asciifolding"]},
                            "edge_words": {"type": "custom", "tokenizer": "words", "filter": ["lowercase", "asciifolding", "edge"]}}}'
# loading: no refresh, async translog; querying: the caches of results are off and max_result_window leaves room for the match sets of the
# correctness checks; the size of the store is not cached (by default _stats/store answers with the size of up to 10 s ago, which after a
# force-merge is the size before it)
ES_SETTINGS='"number_of_shards": 1, "number_of_replicas": 0, "refresh_interval": "-1", "index.translog.durability": "async",
  "index.store.stats_refresh_interval": 0,
  "index.requests.cache.enable": false, "index.queries.cache.enabled": false, "index.max_result_window": 1000000'
ES_ARTICLES="{\"settings\": {$ES_SETTINGS, $ES_ANALYSIS},
  \"mappings\": {\"dynamic\": false, \"_source\": {\"enabled\": false}, \"properties\": {
    \"body\": {\"type\": \"text\", \"analyzer\": \"words\", \"similarity\": \"bm25\"},
    \"created\": {\"type\": \"integer\"}, \"category_id\": {\"type\": \"integer\"}, \"region\": {\"type\": \"integer\"}}}}"
# places: edge n-grams (1-15 letters) of every word at index time, pg_search's edge_ngram(1, 15): a typed prefix is a whole term, and the
# BM25 score is the sum over the n-grams of the typed words, the ranking of the oracle (sql/places_oracle.sql). The typed words are expanded
# into their distinct n-grams by the request renderer (place_ngram_terms, the `:ng` of the other adapters) and analyzed without n-grams: a
# match query on an n-gram analyzer would read the n-grams of a word as synonyms (an OR), not as terms that all must match.
ES_PLACES="{\"settings\": {$ES_SETTINGS, $ES_ANALYSIS},
  \"mappings\": {\"dynamic\": false, \"_source\": {\"enabled\": false}, \"properties\": {
    \"name\": {\"type\": \"text\", \"analyzer\": \"edge_words\", \"search_analyzer\": \"words\", \"similarity\": \"bm25\"},
    \"text\": {\"type\": \"text\", \"analyzer\": \"edge_words\", \"search_analyzer\": \"words\", \"similarity\": \"bm25\"},
    \"country_code\": {\"type\": \"keyword\"}, \"population\": {\"type\": \"integer\"}}}}"

# the configuration directory of a node: the three files an Elasticsearch needs, with the settings of the run (the package's own live in /etc)
x_conf() {
  mkdir -p "$ES_DIR/conf" "$ES_DIR/tmp"
  cat >"$ES_DIR/conf/elasticsearch.yml" <<EOF
path.data: $ES_DIR/data
path.logs: $ES_DIR/logs
http.port: $ES_PORT
transport.port: $(( ES_PORT + 1 ))
network.host: 127.0.0.1
discovery.type: single-node
node.processors: $(cpu_count "${CPUS:-0-$(( $(nproc) - 1 ))}")
xpack.security.enabled: false
xpack.ml.enabled: false
ingest.geoip.downloader.enabled: false
cluster.routing.allocation.disk.threshold_enabled: false
EOF
  cat >"$ES_DIR/conf/jvm.options" <<EOF
-Djava.io.tmpdir=$ES_DIR/tmp
-XX:+ExitOnOutOfMemoryError
-XX:ErrorFile=$ES_DIR/logs/hs_err_pid%p.log
-Xlog:all=warning:stderr:utctime,level,tags
-Dfile.encoding=UTF-8
-Djna.nosys=true
EOF
  cat >"$ES_DIR/conf/log4j2.properties" <<'EOF'
status = error
appender.console.type = Console
appender.console.name = console
appender.console.layout.type = PatternLayout
appender.console.layout.pattern = [%d{ISO8601}][%-5p][%-25c{1.}] %marker%m%n
rootLogger.level = info
rootLogger.appenderRef.console.ref = console
EOF
}
x_start() {
  mkdir -p "$ES_DIR"
  local i
  x_conf
  ES_PATH_CONF=$ES_DIR/conf ES_JAVA_OPTS="-Xms${ES_HEAP:-${TIER_SHBUF[$TIER]%B}} -Xmx${ES_HEAP:-${TIER_SHBUF[$TIER]%B}} -XX:-AlwaysPreTouch" \
    limited "$ES_HOME/bin/elasticsearch" -d -p "$ES_DIR/pid" >"$ES_DIR/start.log" 2>&1
  for (( i = 0; i < 180; i++ )); do
    curl -fs "$URL/_cluster/health?wait_for_status=yellow" >/dev/null 2>&1 && return 0
    sleep 1
  done
  tail -n 20 "$ES_DIR/start.log" "$ES_DIR/logs/"*.log >&2
  return 1
}
x_stop() {
  local pid i
  pid=$(cat "$ES_DIR/pid" 2>/dev/null) || return 0
  kill "$pid" 2>/dev/null || true
  for (( i = 0; i < 120; i++ )); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
  rm -rf "$ES_DIR"
}
x_pid() { cat "$ES_DIR/pid"; }

# METHOD PATH [JSON]
es() { curl -fsS -H 'Content-Type: application/json' -X "$1" "$URL$2" ${3:+--data-binary "$3"} >/dev/null; }
# stdin: NDJSON documents whose first key is "id" (field 4 when split on quotes), indexed into $1; the response must say errors:false
es_bulk() {
  local out; out=$(mktemp -p "$ES_DIR")
  awk -F'"' '{ print "{\"index\":{\"_id\":\"" $4 "\"}}"; print }' | curl -fsS -H 'Content-Type: application/x-ndjson' -X POST "$URL/$1/_bulk" --data-binary @- >"$out"
  grep -q '"errors":false' "$out"; local rc=$?
  rm -f "$out"; return $rc
}
# INDEX FILE: the file in 4 parts, each in requests of 5000 documents, the 4 parts at the same time (the load generators, on the load CPUs)
es_load() {
  export -f es_bulk; export URL ES_DIR
  local tmp=$ES_DIR/parts p; mkdir -p "$tmp"
  split -n l/4 "$2" "$tmp/p."
  for p in "$tmp"/p.*; do loadgen env SHELL=/bin/bash split -l 5000 --filter="es_bulk $1" "$p" & done
  wait
  rm -rf "$tmp"
}
# INDEX FILE CREATE_JSON: the index built from the documents: bulk load + refresh (the build time), then the force-merge to one
# segment; $RUN/build.ms gets `build ms, merge ms, size before the merge`
x_build_index() {
  local t0 t1 t2 pre
  es PUT "/$1" "$3"
  t0=$(now_ms)
  es_load "$1" "$2"
  es POST "/$1/_refresh"
  t1=$(now_ms)
  pre=$(x_size "$1")
  es POST "/$1/_forcemerge?max_num_segments=1"
  es POST "/$1/_flush?force=true"
  t2=$(now_ms)
  es PUT "/$1/_settings" '{"index": {"refresh_interval": "1s"}}'
  echo "$(( t1 - t0 )) $(( t2 - t1 )) $pre" >"$RUN/build.ms"
}
x_build() { x_build_index docs "$1" "$ES_ARTICLES"; }
x_build_places() { x_build_index places "$1" "$ES_PLACES"; }
x_size() { curl -fsS "$URL/${1:-docs}/_stats/store" | grep -m1 -oP '"primaries":\{"store":\{"size_in_bytes":\K[0-9]+' | head -1; }

# articles: the requests of a class, `path <TAB> JSON`. A `join` is the filter on the denormalized `region` of the category (Elasticsearch has no
# join at query time; the region is a field of the document, set at load time), the other kinds are the same query with a different frame.
RENDER=$(cat <<'SQL'
CREATE FUNCTION pg_temp.es_m(mode text, terms text[], op text) RETURNS jsonb LANGUAGE sql AS $$
  SELECT jsonb_build_object('match', jsonb_build_object('body', jsonb_build_object('query', array_to_string(terms, ' '), 'operator', op)))
$$;
CREATE FUNCTION pg_temp.es_q(mode text, terms text[]) RETURNS jsonb LANGUAGE sql AS $$
  SELECT CASE mode
    WHEN 'and'    THEN pg_temp.es_m(mode, terms, 'and')
    WHEN 'or'     THEN pg_temp.es_m(mode, terms, 'or')
    WHEN 'phrase' THEN jsonb_build_object('match_phrase', jsonb_build_object('body', array_to_string(terms, ' ')))
    WHEN 'not'    THEN jsonb_build_object('bool', jsonb_build_object('must', pg_temp.es_m(mode, terms[:cardinality(terms) - 1], 'and'),
                         'must_not', jsonb_build_object('match', jsonb_build_object('body', terms[cardinality(terms)]))))
    WHEN 'prefix' THEN jsonb_build_object('prefix', jsonb_build_object('body', jsonb_build_object('value', terms[1], 'rewrite', 'top_terms_blended_freqs_1024')))
    WHEN 'fuzzy'  THEN jsonb_build_object('fuzzy', jsonb_build_object('body', jsonb_build_object('value', terms[1], 'fuzziness', 1)))
  END
$$;
CREATE FUNCTION pg_temp.es_f(mode text, terms text[], filt jsonb) RETURNS jsonb LANGUAGE sql AS $$
  SELECT jsonb_build_object('bool', jsonb_build_object('must', pg_temp.es_q(mode, terms), 'filter', filt))
$$;
CREATE FUNCTION pg_temp.es_s(size int, q jsonb) RETURNS text LANGUAGE sql AS $$
  SELECT jsonb_build_object('size', size, '_source', false, 'track_total_hits', false, 'query', q)::text
$$;
CREATE FUNCTION pg_temp.x_req(kind text, mode text, terms text[], cutoff int) RETURNS text LANGUAGE sql AS $$
  SELECT CASE kind
    WHEN 'top10'  THEN '/docs/_search' || E'\t' || pg_temp.es_s(10, pg_temp.es_q(mode, terms))
    WHEN 'count'  THEN '/docs/_count' || E'\t' || jsonb_build_object('query', pg_temp.es_q(mode, terms))::text
    WHEN 'filter' THEN '/docs/_search' || E'\t' || pg_temp.es_s(10, pg_temp.es_f(mode, terms, jsonb_build_object('range', jsonb_build_object('created', jsonb_build_object('gte', cutoff)))))
    WHEN 'join'   THEN '/docs/_search' || E'\t' || pg_temp.es_s(10, pg_temp.es_f(mode, terms, jsonb_build_object('term', jsonb_build_object('region', 3))))
    WHEN 'match'  THEN '/docs/_search' || E'\t' || pg_temp.es_s(1000000, pg_temp.es_q(mode, terms))
  END
$$;
CREATE FUNCTION pg_temp.x_write(op text, doc jsonb) RETURNS text LANGUAGE sql AS $$
  SELECT CASE op WHEN 'del' THEN 'DELETE' ELSE 'PUT' END || E'\t/docs/_doc/' || (doc->>'id')
         || CASE op WHEN 'del' THEN '' ELSE E'\t' || doc::text END
$$;
SQL
)

# places: the typed words go to the same analyzer as the documents (edge n-grams). top: the BM25 of the all-fields text; boost: that plus 3 x the
# score of the name field (a `should` clause with boost 3, the matches are those of the text); filter: a `term` on country_code in the filter
# context; count: `_count`; scan: the matches with population >= 100000 in the filter context (no scoring, `_doc` order) and every id returned.
PLACES_FLAVOR="fold"   # a word is a run of letters and digits with accents folded, as for sift (sql/places_oracle.sql)
PLACES_RENDER=$(cat <<'SQL'
CREATE FUNCTION pg_temp.xp_m(fld text, q text, op text) RETURNS jsonb LANGUAGE sql AS $$
  SELECT jsonb_build_object('match', jsonb_build_object(fld, jsonb_build_object('query', place_ngram_terms(q), 'operator', op)))
$$;
CREATE FUNCTION pg_temp.xp_s(size int, q jsonb) RETURNS text LANGUAGE sql AS $$
  SELECT jsonb_build_object('size', size, '_source', false, 'track_total_hits', false, 'query', q)::text
$$;
CREATE FUNCTION pg_temp.xp_req(wl text, q text) RETURNS text LANGUAGE sql AS $$
  SELECT CASE wl
    WHEN 'match'  THEN '/places/_search' || E'\t' || pg_temp.xp_s(1000000, pg_temp.xp_m('text', q, 'and'))
    WHEN 'top'    THEN '/places/_search' || E'\t' || pg_temp.xp_s(20, pg_temp.xp_m('text', q, 'and'))
    WHEN 'boost'  THEN '/places/_search' || E'\t' || pg_temp.xp_s(20, jsonb_build_object('bool', jsonb_build_object('must', pg_temp.xp_m('text', q, 'and'),
                         'should', jsonb_set(pg_temp.xp_m('name', q, 'or'), '{match,name,boost}', '3'))))
    WHEN 'filter' THEN '/places/_search' || E'\t' || pg_temp.xp_s(20, jsonb_build_object('bool', jsonb_build_object('must', pg_temp.xp_m('text', q, 'and'),
                         'filter', jsonb_build_object('term', jsonb_build_object('country_code', 'US')))))
    WHEN 'count'  THEN '/places/_count' || E'\t' || jsonb_build_object('query', pg_temp.xp_m('text', q, 'and'))::text
    WHEN 'scan'   THEN '/places/_search' || E'\t' || (pg_temp.xp_s(10000, jsonb_build_object('bool', jsonb_build_object('filter', jsonb_build_array(pg_temp.xp_m('text', q, 'and'),
                         jsonb_build_object('range', jsonb_build_object('population', jsonb_build_object('gte', 100000)))))))::jsonb || '{"sort": ["_doc"]}')::text
  END
$$;
SQL
)
