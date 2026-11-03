-- a query that mixes AND, OR and NOT, counted from the BM25 index
-- label: `count(*)` of `body @@> 'qaaaf AND (qaatg OR qacyy) NOT qaaab'` on a BM25 index; PostgreSQL is a GIN index on `to_tsvector('simple', body)`
--@ pg
SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('simple', 'qaaaf & (qaatg | qacyy) & !qaaab')
--@ sift
SELECT count(*) FROM docs WHERE body @@> 'qaaaf AND (qaatg OR qacyy) NOT qaaab'
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
