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

# places: `edge_ngram(1, 15)` on the name and on the all-fields text, so a typed prefix is a term; every workload is native.
PGS_TEXT="($PLACES_TEXT)::pdb.alias('ptext')" PGS_NAME="($PLACES_NAME)::pdb.alias('pname')"
PLACES_FLAVOR="split"
PLACES_SETUP="CREATE EXTENSION IF NOT EXISTS pg_search CASCADE"
PLACES_INDEX="CREATE INDEX places_$ENGINE ON places USING bm25 (id, ($PLACES_NAME::pdb.edge_ngram(1, 15, 'alias=pname')), ($PLACES_TEXT::pdb.edge_ngram(1, 15, 'alias=ptext')))"
PLACES_INDEXES=places_$ENGINE
P_match="SELECT id FROM places WHERE $PGS_TEXT &&& :q"
P_top="SELECT id FROM places WHERE $PGS_TEXT &&& :q ORDER BY pdb.score(id) DESC LIMIT 20"
P_boost="SELECT id FROM places WHERE $PGS_TEXT &&& :q AND (($PGS_NAME ||| :q::pdb.boost(3)) OR ($PGS_TEXT &&& :q)) ORDER BY pdb.score(id) DESC LIMIT 20"
P_filter="SELECT id FROM places WHERE $PGS_TEXT &&& :q AND country_code = 'US' ORDER BY pdb.score(id) DESC LIMIT 20"
P_count="SELECT count(*) FROM places WHERE $PGS_TEXT &&& :q"
P_scan="SELECT id FROM places WHERE $PGS_TEXT &&& :q AND population >= 100000"
