-- two rarer terms of which a document holds one, counted from the BM25 index
-- label: `count(*)` of `body @@> 'qaatg OR qacyy'` (ranks 500 and 2000: 0.8% and 0.4% of the documents) on a BM25 index; PostgreSQL is a GIN index on `to_tsvector('simple', body)`
--@ pg
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple', 'qaatg | qacyy')
--@ sift
SELECT count(*) FROM docs WHERE body @@> 'qaatg OR qacyy'
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
