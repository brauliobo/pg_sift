-- a BM25 index with a stemmer (a match that is the call sift.match_idx in the plan) and a btree on another column of the same question: the bitmap heap scan does not analyze the document of each row it fetches again
-- label: the count of `body @@> 'qaaaf' AND category_id = 3` on `USING sift (body) WITH (stemmer = 'english')` and a btree on `category_id` of a copy of the documents (the setup); PostgreSQL is a GIN index on `to_tsvector('english', body)` and the same btree. The statement returns the count
--@ setup
DROP TABLE IF EXISTS docs_rc;
CREATE TABLE docs_rc AS SELECT id, body, category_id FROM docs;
ALTER TABLE docs_rc ADD PRIMARY KEY (id);
CREATE INDEX docs_rc_gin ON docs_rc USING gin (to_tsvector('english', body));
CREATE INDEX docs_rc_category ON docs_rc (category_id);
CREATE INDEX docs_rc_sift ON docs_rc USING sift (body) WITH (stemmer = 'english');
VACUUM (FREEZE, ANALYZE) docs_rc
--@ pg
SELECT count(*) FROM docs_rc WHERE to_tsvector('english', body) @@ to_tsquery('english', 'qaaaf') AND category_id = 3
--@ sift
SELECT count(*) FROM docs_rc WHERE body @@> 'qaaaf' AND category_id = 3
