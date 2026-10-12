# rum over a stored tsvector column ('simple' config); top-k ordered by the index via <=>.
REQUIRES=rum
SETUP="CREATE EXTENSION IF NOT EXISTS rum;
ALTER TABLE docs ADD COLUMN tsv tsvector GENERATED ALWAYS AS (to_tsvector('simple', body)) STORED;"
INDEX="CREATE INDEX docs_fts ON docs USING rum (tsv rum_tsvector_ops);"
RENDER=$(cat <<'SQL'
CREATE OR REPLACE FUNCTION bench_pred(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT 'docs.tsv @@ ' || quote_literal(tsq(mode, terms)) || '::tsquery' $$;
CREATE OR REPLACE FUNCTION bench_rank(mode text, terms text[]) RETURNS text LANGUAGE sql AS
  $$ SELECT 'docs.tsv <=> ' || quote_literal(tsq(mode, terms)) || '::tsquery' $$;
SQL
)
