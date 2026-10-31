-- the build of a BM25 index with the English stemmer, against the build of the GIN index of the English configuration
-- label: `CREATE INDEX .. USING sift (body) WITH (stemmer = 'en')` on the documents of the dataset (one backend, `max_parallel_maintenance_workers` as the run); PostgreSQL builds `USING gin (to_tsvector('english', body))` on the same rows. The words of the vocabulary are stemmed once each
--@ prepare
CREATE TABLE docs AS SELECT * FROM c_docs.docs;
VACUUM (FREEZE, ANALYZE) docs
--@ load
CREATE INDEX docs_gin_en ON docs USING gin (to_tsvector('english', body))
--@ load_sift
CREATE INDEX docs_sift_en ON docs USING sift (body) WITH (stemmer = 'en')
--@ check
SELECT count(*) FROM sift.segments('docs_sift_en')
