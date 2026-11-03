-- a ranked search of two terms that a document must both hold: the page index of a superblock is read by offset words
-- label: the top 20 of `body @@> 'qaaaf qaadw'` (ranks 5 and 100: 16% and 2.1% of the documents) by `sift.score(ctid)` on a BM25 index; PostgreSQL ranks a GIN match with `ts_rank_cd`. The two rankings differ, so the statement returns the number of rows of the page (20)
--@ pg
SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('simple', 'qaaaf & qaadw') ORDER BY ts_rank_cd(tsv, to_tsquery('simple', 'qaaaf & qaadw')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs WHERE body @@> 'qaaaf qaadw' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body)
