-- a ranked search filtered by a column that follows the heap order (a creation date of rows appended over time): the superblocks outside the range are not read
-- label: the top 20 of `body @@> 'qaaaf OR qaatg' AND created_at >= '2025-12-01'` (the last 1/24 of the dates) on `USING sift (body) INCLUDE (category_id, created_at)` of a copy of the documents in the order of `created_at`; PostgreSQL filters the GIN match through the index on `created_at`. The statement returns the number of rows of the page (20)
--@ setup
DROP TABLE IF EXISTS docs_recent;
CREATE TABLE docs_recent AS SELECT id, body, category_id, created_at, tsv FROM docs ORDER BY created_at;
ALTER TABLE docs_recent ADD PRIMARY KEY (id);
CREATE INDEX docs_recent_tsv ON docs_recent USING gin (tsv);
CREATE INDEX docs_recent_created ON docs_recent (created_at);
CREATE INDEX docs_recent_sift ON docs_recent USING sift (body) INCLUDE (category_id, created_at);
VACUUM (FREEZE, ANALYZE) docs_recent
--@ pg
SELECT count(*) FROM (SELECT id FROM docs_recent WHERE tsv @@ to_tsquery('simple', 'qaaaf | qaatg') AND created_at >= '2025-12-01' ORDER BY ts_rank_cd(tsv, to_tsquery('simple', 'qaaaf | qaatg')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs_recent WHERE body @@> 'qaaaf OR qaatg' AND created_at >= '2025-12-01' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
