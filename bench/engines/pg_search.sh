# ParadeDB pg_search (Tantivy inside Postgres), the prebuilt PG18 package unpacked by bench/bench setup.
# Default tokenizer: lowercase, no stemming. created/category_id are indexed so filters push down.
REQUIRES=pg_search
SETUP="CREATE EXTENSION IF NOT EXISTS pg_search CASCADE;"
INDEX="CREATE INDEX docs_fts ON docs USING bm25 (id, body, created, category_id);"
PRED="CASE mode
    WHEN 'and'    THEN format('docs.body &&& %L', array_to_string(terms, ' '))
    WHEN 'or'     THEN format('docs.body ||| %L', array_to_string(terms, ' '))
    WHEN 'phrase' THEN format('docs.body ### %L', array_to_string(terms, ' '))
    WHEN 'not'    THEN format('docs.id @@@ paradedb.boolean(must => ARRAY[paradedb.match(%L, %L, conjunction_mode => true)], must_not => ARRAY[paradedb.term(%L, %L)])',
                              'body', array_to_string(terms[:cardinality(terms) - 1], ' '), 'body', terms[cardinality(terms)])
    WHEN 'prefix' THEN format('docs.id @@@ paradedb.regex(%L, %L)', 'body', terms[1] || '.*')
    WHEN 'fuzzy'  THEN format('docs.id @@@ paradedb.match(%L, %L, distance => 1)', 'body', terms[1])
  END"
RANK="CASE WHEN bench_pred(mode, terms) IS NOT NULL THEN 'paradedb.score(docs.id) DESC' END"
