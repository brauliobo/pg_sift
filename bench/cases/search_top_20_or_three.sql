-- a ranked OR of three terms: the candidates of a block come from the terms' own groups
-- label: the top 20 of `body @@> 'qaaaf OR qaadw OR qaatg'` (ranks 5, 100 and 500) by `sift.score(ctid)` on a BM25 index; PostgreSQL ranks a GIN match with `ts_rank_cd`. The statement returns the number of rows of the page (20)
--@ pg
SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('simple', 'qaaaf | qaadw | qaatg') ORDER BY ts_rank_cd(tsv, to_tsquery('simple', 'qaaaf | qaadw | qaatg')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs WHERE body @@> 'qaaaf OR qaadw OR qaatg' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
