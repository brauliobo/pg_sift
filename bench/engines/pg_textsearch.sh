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
