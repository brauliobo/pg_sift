# pg_textsearch (Timescale) BM25: the `bm25` access method over a PostgreSQL text search configuration, built from the
# pinned release by bench/bench setup. text_config = 'simple' is the analysis of gin and rum (the PG parser, lowercase,
# no stemming, no stopwords), and the database default_text_search_config is 'simple' too because `@@` parses the
# indexed column with it. enable_seqscan is off in its database, as the extension's README suggests to get the index
# plans: with it on the planner counts a disjunction of common words with a sequential scan that tokenizes every
# document (7 qps for count(*) at 25,000 articles, 276 with it off). Ranking is `ORDER BY body <@> to_bm25query(terms, 'docs_fts')` (BM25 with k1 = 1.2, b = 0.75,
# negative scores, block-max WAND with LIMIT); a query is a bag of terms, so the score of an AND, a phrase or a NOT is
# the BM25 sum of their positive terms. Boolean and phrase matching is the tsquery `@@` on the same index (phrase
# positions are rechecked against the heap); prefix terms exist as `:*` but are not scored, and there is no fuzzy match.
# A ranked disjunction needs no filter (the scan only returns documents that contain a term). The other ranked queries
# combine the tsquery filter with the ranking, which the extension does not evaluate as one index scan: as the plain
# `WHERE body @@ q ORDER BY body <@> ...` the planner chooses a sequential scan that tokenizes every document (3 s per
# query at 25,000 articles), so they are written as the extension's documented plan, the filter through the index
# and the standalone scores of its few matches sorted (a derived table the planner cannot flatten). The score is
# `(body <@> query) * 1` there: a bare `ORDER BY body <@> query` over the `@@` scan takes the index scan's current score,
# which that scan does not compute (the matches then come in index order, not by BM25: 53 of the first 200 AND queries have
# more than 10 matches and returned an unranked top-10); the expression makes the planner score the matches standalone.
REQUIRES=pg_textsearch
ORACLE_ACCENTS=${ORACLE_ACCENTS:-keep} ORACLE_TOKENS=${ORACLE_TOKENS:-pg}   # the PG parser tokens, like gin (steps/oracle.sh)
SETUP=$(cat <<'SQL'
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
DO $$ BEGIN
  EXECUTE format('ALTER DATABASE %I SET default_text_search_config = %L', current_database(), 'pg_catalog.simple');
  EXECUTE format('ALTER DATABASE %I SET enable_seqscan = off', current_database());
END $$;
SQL
)
INDEX="CREATE INDEX docs_fts ON docs USING bm25 (body) WITH (text_config = 'simple');"
PRED="'docs.body @@ ' || quote_literal(tsq(mode, terms)) || '::tsquery'"
SOURCE="CASE WHEN mode = 'or' THEN 'docs' ELSE '(SELECT * FROM docs WHERE ' || bench_pred(mode, terms) || ' OFFSET 0) docs' END"
RANK="CASE WHEN mode IN ('and', 'or', 'phrase', 'not') THEN format(CASE mode WHEN 'or' THEN 'docs.body <@> to_bm25query(%L, %L)' ELSE '(docs.body <@> to_bm25query(%L, %L)) * 1' END,
    array_to_string(CASE mode WHEN 'not' THEN terms[:cardinality(terms) - 1] ELSE terms END, ' '), 'docs_fts') END"

# places: the native index is a `bm25` index on the all-fields text; a typed prefix is the prefix tsquery `'gra':* & 'ho':*`, which
# filters through the index (count, unordered scan). The extension has no prefix scoring (a text search configuration has no edge
# n-grams) and no field boost, so the ranked workloads (top, filter, boost) are an emulation built next to the native index, in the
# same database and reported as the `emulation` build: `bm25` indexes on stored columns holding the edge n-grams (1-15 letters) of
# every word (pg_search's analysis, so a typed prefix is a whole term); the matches `ng_text @@ plainto_tsquery(typed)` (a derived
# table) are scored standalone with the distinct n-grams of the typed words (`* 1`, see above); the boost is the all-fields score plus
# 3 x the score of a second index on the name field.
PLACES_FLAVOR="split" PLACES_FLAVOR_MATCH="pg"
PLACES_SETUP="CREATE EXTENSION IF NOT EXISTS pg_textsearch; ALTER DATABASE places_$ENGINE SET default_text_search_config = 'pg_catalog.simple'; ALTER DATABASE places_$ENGINE SET enable_seqscan = off"
PLACES_INDEX="CREATE INDEX places_$ENGINE ON places USING bm25 (($PLACES_TEXT)) WITH (text_config = 'simple')"
PLACES_INDEXES=places_$ENGINE
PLACES_SETUP2="ALTER TABLE places ADD COLUMN ng_text text GENERATED ALWAYS AS (place_ngrams($PLACES_TEXT)) STORED, ADD COLUMN ng_name text GENERATED ALWAYS AS (place_ngrams($PLACES_NAME)) STORED"
PLACES_INDEX2="CREATE INDEX places_pg_textsearch_ng ON places USING bm25 (ng_text) WITH (text_config = 'simple'); CREATE INDEX places_pg_textsearch_ngname ON places USING bm25 (ng_name) WITH (text_config = 'simple')"
PLACES_INDEXES2="places_pg_textsearch_ng places_pg_textsearch_ngname"
NG_SCAN="SELECT * FROM places WHERE ng_text @@ plainto_tsquery('simple', :q)"
NG_TEXT="(ng_text <@> to_bm25query(:ng, 'places_pg_textsearch_ng'))" NG_NAME="(ng_name <@> to_bm25query(:ng, 'places_pg_textsearch_ngname'))"
P_match="SELECT id FROM places WHERE $PLACES_TEXT @@ :tq::tsquery"
P_top="SELECT id FROM ($NG_SCAN OFFSET 0) p ORDER BY $NG_TEXT * 1 LIMIT 20"
P_boost="SELECT id FROM ($NG_SCAN OFFSET 0) p ORDER BY $NG_TEXT + 3 * $NG_NAME LIMIT 20"
P_filter="SELECT id FROM ($NG_SCAN AND country_code = 'US' OFFSET 0) p ORDER BY $NG_TEXT * 1 LIMIT 20"
P_count="SELECT count(*) FROM places WHERE $PLACES_TEXT @@ :tq::tsquery"
P_scan="SELECT id FROM places WHERE $PLACES_TEXT @@ :tq::tsquery AND population >= 100000"
