-- `jsonb_typeof(doc -> 'v')` as a group key over a key of mixed types (a number, a string, a json null or no key): the type of each document is read from the leaf, not the document
-- label: `jsonb_typeof(doc -> 'v'), count(*)` over {rows} documents; the variant `expr_btree` is a heap copy with a b-tree on `jsonb_typeof(doc -> 'v')`
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS jt_docs;
CREATE TABLE jt_docs (id int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO jt_docs SELECT g, CASE g % 4 WHEN 0 THEN jsonb_build_object('v', g % 1000, 'pad', repeat('x', 100))
	WHEN 1 THEN jsonb_build_object('v', 'v_' || g % 50, 'pad', repeat('x', 100)) WHEN 2 THEN jsonb_build_object('v', null, 'pad', repeat('x', 100))
	ELSE jsonb_build_object('w', g % 1000, 'pad', repeat('x', 100)) END
FROM generate_series(1, {rows}) g;
CREATE INDEX jt_docs_col ON jt_docs USING sift_col (id, doc) WITH (paths = 'doc.v text');
VACUUM (FREEZE, ANALYZE) jt_docs;
DROP SCHEMA IF EXISTS c_jsonb_typeof_groupby__expr_btree CASCADE;
CREATE SCHEMA c_jsonb_typeof_groupby__expr_btree;
CREATE TABLE c_jsonb_typeof_groupby__expr_btree.jt_docs WITH (autovacuum_enabled = false) AS SELECT * FROM jt_docs;
CREATE INDEX jt_docs_typeof ON c_jsonb_typeof_groupby__expr_btree.jt_docs ((jsonb_typeof(doc -> 'v')));
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_jsonb_typeof_groupby__expr_btree.jt_docs
--@ pg
SELECT jsonb_typeof(doc -> 'v') AS t, count(*) AS n FROM jt_docs GROUP BY 1 ORDER BY 1
