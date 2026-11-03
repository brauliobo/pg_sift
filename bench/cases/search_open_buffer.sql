-- a ranked search while the write buffer of the index holds the last inserts: 3,000 documents, which every search matches one by one
-- label: the top 20 of `body @@> 'qaaaf qaadw'` (ranks 5 and 100: 16% and 2.1% of the documents) on a copy of the documents whose last 3,000 are inserted after the index is built and stay in the open write buffer (the setup); PostgreSQL is a GIN index on `to_tsvector('simple', body)` of the same rows, with the same 3,000 inserted. The statement returns the number of rows of the page (20)
--@ setup
DROP TABLE IF EXISTS docs_buf;
CREATE TABLE docs_buf AS SELECT id, body FROM docs WHERE id > 3000;
ALTER TABLE docs_buf ADD PRIMARY KEY (id);
CREATE INDEX docs_buf_gin ON docs_buf USING gin (to_tsvector('simple', body));
CREATE INDEX docs_buf_sift ON docs_buf USING sift (body);
VACUUM (FREEZE, ANALYZE) docs_buf;
INSERT INTO docs_buf SELECT id, body FROM docs WHERE id <= 3000
--@ pg
SELECT count(*) FROM (SELECT id FROM docs_buf WHERE to_tsvector('simple', body) @@ to_tsquery('simple', 'qaaaf & qaadw') ORDER BY ts_rank_cd(to_tsvector('simple', body), to_tsquery('simple', 'qaaaf & qaadw')) DESC, id LIMIT 20) t
--@ sift
SELECT count(*) FROM (SELECT id FROM docs_buf WHERE body @@> 'qaaaf qaadw' ORDER BY sift.score(ctid) DESC, id LIMIT 20) t
