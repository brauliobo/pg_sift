-- the merge of segments of a BM25 index by workers that write ranges of terms read from the input segments
-- label: `sift.merge('docs_sift', true)` of four segments of a quarter of the documents each (an index that took four batches, `merge_fanin = 8` so that none is merged on the way), with 0, 2 and 4 parallel maintenance workers (lanes workers_N; the lane sift is the run's CASES_WORKERS): the participants write the terms of the merged segment in ranges, read from the input segments. PostgreSQL's lane flushes the pending list of a GIN index that took the same four batches. The merged segment is the one of a single process, byte for byte (merge_ranges, test/sql)
--@ prepare
CREATE TABLE docs AS SELECT * FROM c_docs.docs WHERE false;
CREATE INDEX docs_gin ON docs USING gin (tsv) WITH (fastupdate = on, gin_pending_list_limit = 2000000);
CREATE INDEX docs_sift ON docs USING sift (body) WITH (merge_fanin = 8);
INSERT INTO docs SELECT * FROM c_docs.docs WHERE id % 4 = 0;
SELECT sift.promote('docs_sift');
INSERT INTO docs SELECT * FROM c_docs.docs WHERE id % 4 = 1;
SELECT sift.promote('docs_sift');
INSERT INTO docs SELECT * FROM c_docs.docs WHERE id % 4 = 2;
SELECT sift.promote('docs_sift');
INSERT INTO docs SELECT * FROM c_docs.docs WHERE id % 4 = 3;
SELECT sift.promote('docs_sift')
--@ load
SELECT gin_clean_pending_list('docs_gin')
--@ load_sift
SELECT sift.merge('docs_sift', true)
--@ check
SELECT 'segments ' || count(*) FROM sift.segments('docs_sift') WHERE kind = 'immutable'
