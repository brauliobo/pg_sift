-- single-row inserts into a table with a language index (the stemmer of a PostgreSQL text search configuration), against the GIN index of the same configuration
-- label: 3,000 single-row `INSERT`s of documents into a heap table of 100,000 documents with its primary key; pg_sift: `USING sift (body) WITH (stemmer = 'german')`, native: `USING gin (to_tsvector('german', body))`
--@ prepare
CREATE TABLE docs_w AS SELECT id, body FROM c_docs.docs WHERE id <= 100000;
ALTER TABLE docs_w ADD PRIMARY KEY (id);
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX docs_w_idx ON docs_w USING sift (body) WITH (stemmer = 'german'); ELSE CREATE INDEX docs_w_idx ON docs_w USING gin (to_tsvector('german', body)); END IF; END $$;
VACUUM ANALYZE docs_w
--@ load
DO $$ BEGIN FOR i IN 1..3000 LOOP INSERT INTO docs_w SELECT 10000000 + i, body FROM docs_w WHERE id = 1 + (i * 37) % 100000; END LOOP; END $$
--@ check
SELECT count(*) FROM docs_w
