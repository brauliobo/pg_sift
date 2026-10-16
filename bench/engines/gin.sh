# Built-in GIN over a stored tsvector column ('simple' config: no stemming, no stopwords).
# Ranking uses ts_rank_cd, which must detoast and score every matching row.
ORACLE_ACCENTS=${ORACLE_ACCENTS:-keep} ORACLE_TOKENS=${ORACLE_TOKENS:-pg}   # indexes the raw PG parser tokens (steps/oracle.sh)
SETUP="ALTER TABLE docs ADD COLUMN tsv tsvector GENERATED ALWAYS AS (to_tsvector('simple', body)) STORED;"
INDEX="CREATE INDEX docs_fts ON docs USING gin (tsv);"
PRED="'docs.tsv @@ ' || quote_literal(tsq(mode, terms)) || '::tsquery'"
RANK="'ts_rank_cd(docs.tsv, ' || quote_literal(tsq(mode, terms)) || '::tsquery) DESC'"
