# Built-in GIN over a stored tsvector column ('simple' config: no stemming, no stopwords).
# Ranking uses ts_rank_cd, which must detoast and score every matching row.
REQUIRES=
SETUP="ALTER TABLE docs ADD COLUMN tsv tsvector GENERATED ALWAYS AS (to_tsvector('simple', body)) STORED;"
INDEX="CREATE INDEX docs_fts ON docs USING gin (tsv);"
RENDER=$(cat <<'SQL'
CREATE OR REPLACE FUNCTION bench_pred(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT 'docs.tsv @@ ' || quote_literal(tsq(mode, terms)) || '::tsquery' $$;
CREATE OR REPLACE FUNCTION bench_rank(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT 'ts_rank_cd(docs.tsv, ' || quote_literal(tsq(mode, terms)) || '::tsquery) DESC' $$;
SQL
)
