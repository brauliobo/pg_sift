-- a ranked search whose filter is a column kept in the BM25 index (INCLUDE), against the GIN match filtered by the heap
-- label: the top 20 of `body @@> 'qaaaf OR qaatg' AND created_at >= '2025-06-01'` on `USING sift (body) INCLUDE (category_id, created_at)`; PostgreSQL filters the GIN match on the heap. The statement returns the number of rows of the page (20)
--@ pg
SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('simple', 'qaaaf | qaatg') AND created_at >= '2025-06-01' ORDER BY ts_rank_cd(tsv, to_tsquery('simple', 'qaaaf | qaatg')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs WHERE body @@> 'qaaaf OR qaatg' AND created_at >= '2025-06-01' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
--@ setup
DROP INDEX IF EXISTS docs_sift;
CREATE INDEX docs_sift ON docs USING sift (body) INCLUDE (category_id, created_at)
