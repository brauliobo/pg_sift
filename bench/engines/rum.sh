# rum over the same stored tsvector column as gin ('simple' config); top-k ordered by the index via <=>.
. "$BENCH/engines/gin.sh"
REQUIRES=rum
SETUP="CREATE EXTENSION IF NOT EXISTS rum; $SETUP"
INDEX="CREATE INDEX docs_fts ON docs USING rum (tsv rum_tsvector_ops);"
RANK="'docs.tsv <=> ' || quote_literal(tsq(mode, terms)) || '::tsquery'"
