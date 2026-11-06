-- `doc @> '{"b": "x", "flag": true}'`: a string member is a test of its leaf and a boolean on an astext leaf is its text and the JSON type boolean, the scan runs both on the leaves
-- label: `SELECT count(*), sum(n) FROM jc_fact WHERE doc @> '{"b": "x", "flag": true}'` over 600,000 documents, a GIN index on doc for the native plan
-- ordered: yes
--@ setup
CREATE TABLE jc_fact AS SELECT g AS id, g % 53 AS grp, jsonb_build_object('b', (array['x', 'y', 'z', 'w', 'v'])[1 + g % 5], 'flag', CASE g % 4 WHEN 0 THEN to_jsonb(true) WHEN 1 THEN to_jsonb(false) WHEN 2 THEN to_jsonb('true'::text) ELSE 'null'::jsonb END, 'n', g % 1000) AS doc FROM generate_series(1, {rows} * 3) g;
CREATE INDEX jc_fact_gin ON jc_fact USING gin (doc);
CREATE INDEX jc_fact_col ON jc_fact USING sift_col (id, grp, doc) WITH (key_cut = grp, paths = 'doc.b astext, doc.flag astext, doc.n int4');
VACUUM ANALYZE jc_fact
--@ pg
SELECT count(*) AS n, sum((doc ->> 'n')::int) AS total FROM jc_fact WHERE doc @> '{"b": "x", "flag": true}'
