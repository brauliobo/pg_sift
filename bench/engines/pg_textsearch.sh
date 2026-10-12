# pg_textsearch (Timescale) BM25: ranked disjunction only. The index cannot answer
# conjunction, phrase, NOT or count(*), so those render NULL (= unsupported).
REQUIRES=pg_textsearch
SETUP="CREATE EXTENSION IF NOT EXISTS pg_textsearch;"
INDEX="CREATE INDEX docs_fts ON docs USING bm25 (body) WITH (text_config = 'simple');"
RENDER=$(cat <<'SQL'
CREATE OR REPLACE FUNCTION bench_pred(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT NULL::text $$;
CREATE OR REPLACE FUNCTION bench_rank(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT CASE mode WHEN 'or' THEN format('docs.body <@> to_bm25query(%L, %L)', array_to_string(terms, ' '), 'docs_fts') END $$;
SQL
)
