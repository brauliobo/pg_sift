-- VACUUM of a BM25 index after 1% of the documents were inserted, updated and deleted: the dead postings of each term are counted by reading the postings of a LIST term one by one against a flat table of the dead rows of each superblock
-- label: `VACUUM` of a table of the documents of the run with a BM25 index after an `INSERT`, an `UPDATE` and a `DELETE` of 1% of the rows each (a GIN index of a tsvector in the other lane); the metrics are the time, the instructions and the WAL of the VACUUM
--@ prepare
CREATE TABLE docs AS SELECT * FROM c_docs.docs;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX docs_sift ON docs USING sift (body); ELSE CREATE INDEX docs_gin ON docs USING gin (tsv); END IF; END $$;
VACUUM (FREEZE, ANALYZE) docs;
INSERT INTO docs SELECT id + (SELECT max(id) FROM c_docs.docs), body, category_id, created_at, tsv FROM docs WHERE id % 100 = 7;
UPDATE docs SET body = body || ' qaaaf', tsv = to_tsvector('simple', body || ' qaaaf') WHERE id % 100 = 11;
DELETE FROM docs WHERE id % 100 = 13
--@ load
VACUUM docs
--@ check
SELECT count(*) FROM docs
