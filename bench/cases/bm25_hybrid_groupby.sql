-- a grouped aggregate over the rows of a BM25 match: the match set of the sift index read beside the columns of the sift_col index, against full text search with a GIN of the tsvector
-- label: `GROUP BY status` of `body @@> 'wire switch'` (one row in five); PostgreSQL matches `to_tsvector('simple', body) @@ plainto_tsquery('simple', 'wire switch')`; the variant `gin` is a copy of the rows with a GIN on that tsvector
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS c_bm25_hybrid_groupby__gin CASCADE;
CREATE SCHEMA c_bm25_hybrid_groupby__gin;
CREATE TABLE c_bm25_hybrid_groupby__gin.sf_events (LIKE sf_events) WITH (autovacuum_enabled = false);
INSERT INTO c_bm25_hybrid_groupby__gin.sf_events SELECT * FROM sf_events;
CREATE INDEX sf_events_tsv ON c_bm25_hybrid_groupby__gin.sf_events USING gin (to_tsvector('simple', body));
VACUUM (FREEZE, ANALYZE) c_bm25_hybrid_groupby__gin.sf_events
--@ pg
SELECT status, count(*), sum(amount) FROM sf_events WHERE to_tsvector('simple', body) @@ plainto_tsquery('simple', 'wire switch') GROUP BY 1 ORDER BY 1
--@ sift
SELECT status, count(*), sum(amount) FROM sf_events WHERE body @@> 'wire switch' GROUP BY 1 ORDER BY 1
