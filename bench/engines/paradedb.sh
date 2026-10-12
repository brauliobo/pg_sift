# ParadeDB pg_search (Tantivy inside Postgres), prebuilt PG18 package from bench/fetch.sh paradedb.
# Default tokenizer: lowercase, no stemming. created/prop_id are indexed so filters push down.
REQUIRES=pg_search
SETUP="CREATE EXTENSION IF NOT EXISTS pg_search CASCADE;"
INDEX="CREATE INDEX docs_fts ON docs USING bm25 (id, body, created, prop_id);"
RENDER=$(cat <<'SQL'
CREATE OR REPLACE FUNCTION bench_pred(mode text, terms text[]) RETURNS text LANGUAGE sql AS $$
  SELECT CASE mode
    WHEN 'and'    THEN format('docs.body &&& %L', array_to_string(terms, ' '))
    WHEN 'or'     THEN format('docs.body ||| %L', array_to_string(terms, ' '))
    WHEN 'phrase' THEN format('docs.body ### %L', array_to_string(terms, ' '))
    WHEN 'not'    THEN format('docs.id @@@ paradedb.boolean(must => ARRAY[paradedb.match(%L, %L, conjunction_mode => true)], must_not => ARRAY[paradedb.term(%L, %L)])',
                              'body', array_to_string(terms[:cardinality(terms) - 1], ' '), 'body', terms[cardinality(terms)])
  END
$$;
CREATE OR REPLACE FUNCTION bench_rank(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT 'paradedb.score(docs.id) DESC' $$;
SQL
)
