-- a prefix LIKE of a text column under a grouped aggregate: the test is made once per distinct url, against a b-tree with the pattern operator class
-- label: `GROUP BY status` of `url LIKE 'https://google%'` (one row in twelve); the variant `btree` is a copy of the rows with a b-tree on (url text_pattern_ops)
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS c_text_like_prefix_agg__btree CASCADE;
CREATE SCHEMA c_text_like_prefix_agg__btree;
CREATE TABLE c_text_like_prefix_agg__btree.sf_events (LIKE sf_events) WITH (autovacuum_enabled = false);
INSERT INTO c_text_like_prefix_agg__btree.sf_events SELECT * FROM sf_events;
CREATE INDEX sf_events_url ON c_text_like_prefix_agg__btree.sf_events (url text_pattern_ops);
VACUUM (FREEZE, ANALYZE) c_text_like_prefix_agg__btree.sf_events
--@ pg
SELECT status, count(*), avg(amount) FROM sf_events WHERE url LIKE 'https://google%' GROUP BY 1 ORDER BY 1
