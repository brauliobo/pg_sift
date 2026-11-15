-- The inner or semi lookup join on a text field of the elements of an array tests the keys of the table on the encoded form of each unit; a sift_table runs the test of the
-- element field through the group filter, as a column index does (it used to stop with "leaf is an element column"). The rows must be those of the normal plan, with no worker and with two.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
CREATE TABLE et (id int, grp int, items jsonb, amount int) USING sift_table;
SELECT sift.table_set_paths('et', 'items', 'items[]{category text, qty int4}');
-- no array, an empty array, an element with a missing field and one with a NULL field
INSERT INTO et
SELECT i, i % 7,
       CASE WHEN i % 19 = 0 THEN NULL
            WHEN i % 29 = 0 THEN '[]'::jsonb
            ELSE (SELECT jsonb_agg(CASE WHEN (i + e) % 31 = 0 THEN '{"qty": 1}'::jsonb
                                        WHEN (i + e) % 23 = 0 THEN jsonb_build_object('category', NULL, 'qty', e)
                                        ELSE jsonb_build_object('category', 'cat' || (i + e) % 40, 'qty', e) END)
                  FROM generate_series(1, 1 + i % 4) e) END,
       i % 100
FROM generate_series(1, 6000) i;
VACUUM FULL et;
CREATE TABLE keys (k text, tag int);
INSERT INTO keys VALUES ('cat3', 1), ('cat3', 2), ('cat17', 3), ('cat39', 4), (NULL, 5), ('absent', 6);
CREATE TABLE keys_uniq (k text PRIMARY KEY, tag int);
INSERT INTO keys_uniq VALUES ('cat3', 1), ('cat17', 3), ('cat39', 4), ('absent', 6);
ANALYZE keys; ANALYZE keys_uniq;

CREATE TABLE q (n int, label text, q text);
INSERT INTO q VALUES
	(1, 'a semi join', 'SELECT i.category, count(*) AS n, sum(i.qty) AS units FROM et o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) WHERE EXISTS (SELECT 1 FROM keys m WHERE m.k = i.category) GROUP BY 1 ORDER BY 1'),
	(2, 'an inner join by a unique key', 'SELECT m.tag, count(*) AS n, count(DISTINCT o.id) AS orders FROM et o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) JOIN keys_uniq m ON m.k = i.category GROUP BY 1 ORDER BY 1'),
	(3, 'a semi join and a condition on the table', 'SELECT i.category, count(*) AS n FROM et o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) WHERE o.grp < 3 AND EXISTS (SELECT 1 FROM keys m WHERE m.k = i.category) GROUP BY 1 ORDER BY 1');
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_siftagg_with_text_key_filters(q) $$;
CREATE FUNCTION elem_rows(q text, scan bool, workers int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_with_workers_reset(q, scan, workers) $$;
SELECT n, label, plan_of(q) AS plan, elem_rows(q, false, 0) <> '' AS has_rows, elem_rows(q, true, 0) = elem_rows(q, false, 0) AS same, elem_rows(q, true, 2) = elem_rows(q, false, 0) AS same_workers FROM q ORDER BY n;

DROP FUNCTION plan_of(text);
DROP FUNCTION elem_rows(text, bool, int);
DROP TABLE q, et, keys, keys_uniq;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
