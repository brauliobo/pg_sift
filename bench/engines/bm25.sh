# pgx-bm25 `bm25_native`, loaded from the extracted package via extension_control_path.
# Its analyzer resolves `<language>_stem` in pg_catalog, so a `simple_stem` dictionary
# (simple template) gives it the same no-stemming analysis as the other engines.
REQUIRES=bm25_native
SETUP="CREATE EXTENSION IF NOT EXISTS bm25_native;
SET allow_system_table_mods = on;
CREATE TEXT SEARCH DICTIONARY pg_catalog.simple_stem (TEMPLATE = simple);
RESET allow_system_table_mods;"
INDEX="CREATE INDEX docs_fts ON docs USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id', language = 'simple', stopwords = 'none');"
RENDER=$(cat <<'SQL'
CREATE OR REPLACE FUNCTION bm25_q(mode text, terms text[]) RETURNS jsonb LANGUAGE sql AS $$
  SELECT CASE mode
    WHEN 'and'    THEN bm25_boolean(must => (SELECT array_agg(bm25_term('body', t)) FROM unnest(terms) t))
    WHEN 'or'     THEN bm25_boolean(should => (SELECT array_agg(bm25_term('body', t)) FROM unnest(terms) t))
    WHEN 'phrase' THEN bm25_phrase('body', array_to_string(terms, ' '))
    WHEN 'not'    THEN bm25_boolean(must => (SELECT array_agg(bm25_term('body', t)) FROM unnest(terms[:cardinality(terms) - 1]) t),
                                    must_not => ARRAY[bm25_term('body', terms[cardinality(terms)])])
  END
$$;
CREATE OR REPLACE FUNCTION bench_pred(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT format('docs.body @@@ %L::jsonb', bm25_q(mode, terms)) $$;
CREATE OR REPLACE FUNCTION bench_rank(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT format('docs.body &@@ %L::jsonb', bm25_q(mode, terms)) $$;
SQL
)
