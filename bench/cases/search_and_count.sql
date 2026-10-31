-- two terms that a document must both hold, counted from the BM25 index against the GIN index of a tsvector
-- label: `count(*)` of `body @@> 'qaaab qaaaf'` (the terms of rank 1 and 5 of the corpus: 78% and 16% of the documents) on a BM25 index; PostgreSQL is a GIN index on `to_tsvector('simple', body)`
--@ pg
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple', 'qaaab & qaaaf')
--@ sift
SELECT count(*) FROM docs WHERE body @@> 'qaaab qaaaf'
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
