-- two terms side by side, counted from the positions of the BM25 index
-- label: `count(*)` of `body @@> '"qaabo qaabp"'` (the phrase every 97th document holds, 1% of the documents) on a BM25 index; PostgreSQL is a GIN index on `to_tsvector('simple', body)` and a phrase query
--@ pg
SELECT count(*) FROM docs WHERE tsv @@ phraseto_tsquery('simple', 'qaabo qaabp')
--@ sift
SELECT count(*) FROM docs WHERE body @@> '"qaabo qaabp"'
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
