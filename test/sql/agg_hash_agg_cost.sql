-- PostgreSQL prices a row of a hash aggregate at one operator whatever the groups, though a table past the cache of a core misses the memory for the bucket and the
-- stored key of every row (4 to 7 misses a row at 7 to 40 MB, 3 times the instructions of a table of a few groups: 1,000,000 rows of 500,000 text groups took
-- 1.4 s where the plan was priced 66 ms). The normal plan is priced for them where a scan competes with it: a near-unique key over the whole table takes the scan,
-- as an integer key of the same groups does, while a selective filter that leaves the few groups of the rows it passes keeps the normal plan (agg_key_entries_cost).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET work_mem = '64MB';
CREATE TABLE items (id int PRIMARY KEY, tag text NOT NULL, cat text NOT NULL, amount float8 NOT NULL);
SELECT setseed(0.42);
INSERT INTO items SELECT g, md5(g::text), 'c' || (g % 17), random() * 100 FROM generate_series(1, 200000) g;
CREATE INDEX items_col ON items USING sift_col (id, tag, cat, amount) WITH (order_array_columns = 'off');
VACUUM (FREEZE, ANALYZE) items;

CREATE FUNCTION why(q text) RETURNS TABLE (used boolean, normal_priced_for_groups boolean) LANGUAGE sql AS
$$ SELECT used, normal_cost::numeric > 20000 FROM sift.explain_why(q) LIMIT 1 $$;

-- 200,000 text groups (about 20 MB of table): the scan; the 17 values and a selective filter do not change
SELECT * FROM why('SELECT tag, count(*) AS c FROM items GROUP BY tag ORDER BY c DESC LIMIT 10');
SELECT * FROM why('SELECT cat, count(*) AS c FROM items GROUP BY cat ORDER BY c DESC LIMIT 10');
SELECT * FROM why('SELECT tag, count(*) AS c FROM items WHERE id < 5000 GROUP BY tag ORDER BY c DESC LIMIT 10');

DROP FUNCTION why(text);
DROP TABLE items;
DROP EXTENSION pg_sift;
