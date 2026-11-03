-- the build of a BM25 index on the documents, against the build of the GIN index of a tsvector
-- label: `CREATE INDEX .. USING sift (body)` on the documents of the dataset (one backend, `max_parallel_maintenance_workers` as the run); PostgreSQL builds `USING gin (tsv)` on the same rows. A case of the build, the merge or the maintenance of a BM25 index copies this one
--@ prepare
CREATE TABLE docs AS SELECT * FROM c_docs.docs;
VACUUM (FREEZE, ANALYZE) docs
--@ load
CREATE INDEX docs_gin ON docs USING gin (tsv)
--@ load_sift
CREATE INDEX docs_sift ON docs USING sift (body)
--@ check
SELECT count(*) FROM sift.segments('docs_sift')
