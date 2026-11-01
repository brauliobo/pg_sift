-- an index that takes batches: merge_fanin defaults to 4, so it keeps fewer segments and a statement reads fewer
-- label: the documents of the dataset inserted in batches of 5,000 (one transaction each) into a table with a BM25 index (`merge_fanin` at its default of 4) or a GIN index of a tsvector; the check is the number of segments the index ends with
--@ prepare
CREATE TABLE docs AS SELECT * FROM c_docs.docs WHERE false;
CREATE INDEX docs_gin ON docs USING gin (tsv);
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX docs_sift ON docs USING sift (body); END IF; END $$
--@ load
DO $$ DECLARE n int := (SELECT count(*) FROM c_docs.docs); BEGIN FOR i IN 0..(n - 1) / 5000 LOOP INSERT INTO docs SELECT * FROM c_docs.docs WHERE id > i * 5000 AND id <= (i + 1) * 5000; COMMIT; END LOOP; END $$
--@ check
SELECT count(*) FROM sift.segments('docs_sift')
