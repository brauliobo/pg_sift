-- scalar leaves of a jsonb document as group keys and an aggregate argument (`doc ->> 'channel'`, `doc ->> 'flag'`, `(doc ->> 'score')::float8`) under a key test (`doc ? 'channel'`): the groups are read from the leaves of the paths
-- label: `doc ->> 'channel', doc ->> 'flag'`, `count(*)` and `avg((doc ->> 'score')::float8)` of sf_events; the variant `expr_btree` is a heap copy with a b-tree on the three expressions
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS c_jsonb_scalar_groupby__expr_btree CASCADE;
CREATE SCHEMA c_jsonb_scalar_groupby__expr_btree;
CREATE TABLE c_jsonb_scalar_groupby__expr_btree.sf_events WITH (autovacuum_enabled = false) AS SELECT * FROM sf_events;
CREATE INDEX sf_events_doc_expr ON c_jsonb_scalar_groupby__expr_btree.sf_events ((doc ->> 'channel'), (doc ->> 'flag'), ((doc ->> 'score')::float8));
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_jsonb_scalar_groupby__expr_btree.sf_events
--@ pg
SELECT doc ->> 'channel' AS channel, doc ->> 'flag' AS flag, count(*) AS n, avg((doc ->> 'score')::float8) AS score FROM sf_events WHERE doc ? 'channel' GROUP BY 1, 2 ORDER BY 1, 2
