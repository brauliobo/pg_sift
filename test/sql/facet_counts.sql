-- sift.facet_counts / sift.facet_sql (docs/FIELDS-FACETS.md): the facets of a search, several in one statement, over a table with a sift
-- (BM25) index and a sift_col index. Every case runs three times: the facet function with the column scan forced (the plans that are not
-- SiftAgg disabled), the facet function with the planner's own plan (sift.enable_agg off), and a hand-written GROUP BY of the same
-- snapshot as the oracle; the three results are compared as sets of rows, after the build and after each kind of write.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

CREATE TABLE fc (id serial PRIMARY KEY, title text, brand text, category int, price numeric(10,2), qty int, added date, ts timestamp,
				 active bool, tags text[], pad text DEFAULT repeat('x', 700));
ALTER TABLE fc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO fc (title, brand, category, price, qty, added, ts, active, tags)
SELECT (ARRAY['wireless','wired','portable','compact','classic'])[1 + i % 5] || ' ' || (ARRAY['headphones','speaker','charger','cable'])[1 + i % 4] || ' w' || i % 97,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE (ARRAY['acme','zeta','omni','nova','Ünï','x'])[1 + (hashint4(i) & 2147483647) % 6] END,
	   (hashint4(i + 3) & 2147483647) % 9,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE round(((hashint4(i + 5) & 2147483647) % 60000 / 100.0)::numeric, 2) END,
	   (hashint4(i + 7) & 2147483647) % 13 * 10,
	   date '2024-01-01' + (hashint4(i + 9) & 2147483647) % 500,
	   timestamp '2024-01-01' + ((hashint4(i + 9) & 2147483647) % 500) * interval '1 day' + (i % 24) * interval '1 hour',
	   CASE WHEN i % 13 = 0 THEN NULL ELSE i % 3 = 0 END,
	   CASE WHEN i % 19 = 0 THEN NULL ELSE ARRAY[(ARRAY['red','green','blue'])[1 + i % 3], (ARRAY['s','m','l'])[1 + i % 3], (ARRAY['red','green','blue'])[1 + i % 3]] END
FROM generate_series(1, 12000) i;
CREATE INDEX fc_bm ON fc USING sift (title);
CREATE INDEX fc_col ON fc USING sift_col (brand, category, price, qty, added, ts, active) WITH (write_buffer_limit = 256);
VACUUM (ANALYZE) fc;

-- the plans that are not SiftAgg are disabled for the pushed run; the planner's own choice with sift.enable_agg off is the other
CREATE FUNCTION fc_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('sift.enable_scan', (NOT pushed)::text, true);
	PERFORM set_config('sift.enable_count', (NOT pushed)::text, true);
END $$;

-- the oracle of a terms facet: a GROUP BY of the column, the first sz values by count then value, the rest as one row, NULL as missing
CREATE FUNCTION fc_terms(name text, col text, wh text, sz int DEFAULT NULL, miss bool DEFAULT false) RETURNS text LANGUAGE sql AS $$
	SELECT format($f$ SELECT * FROM (WITH g AS (SELECT (%1$s)::text v, count(*) c FROM fc WHERE %3$s GROUP BY %1$s),
		r AS (SELECT v, c, row_number() OVER (ORDER BY c DESC, (v COLLATE "C")) rn FROM g WHERE v IS NOT NULL)
		SELECT %2$L, 'value', v, NULL, NULL, c FROM r WHERE %4$s IS NULL OR rn <= %4$s
		UNION ALL SELECT %2$L, 'other', NULL, count(*)::text, NULL, sum(c) FROM r WHERE rn > %4$s HAVING count(*) > 0
		UNION ALL SELECT %2$L, 'missing', NULL, NULL, NULL, c FROM g WHERE v IS NULL AND %5$L::bool) t $f$, col, name, wh, coalesce(sz::text, 'NULL'), miss) $$;

-- same / DIFFERENT for the function with the column scan, the function with the planner's plan against the oracle, and how many scans of the
-- function's plan read the match set ('searches', a SiftAgg Search line each)
CREATE FUNCTION fc_check(tbl regclass, q text, facets jsonb, filt text, ref text, hitwh text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; m bigint; plan text := ''; line text; sql text := sift.facet_sql(tbl, 'title', q, facets, filt, true);
BEGIN
	DROP TABLE IF EXISTS fc_a, fc_b, fc_c;
	PERFORM fc_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP plan := plan || line || E'\n'; END LOOP;
	CREATE TEMP TABLE fc_a AS SELECT row_to_json(x)::text AS r FROM sift.facet_counts(tbl, 'title', q, facets, filt, true) x;
	PERFORM fc_mode(false);
	CREATE TEMP TABLE fc_b AS SELECT row_to_json(x)::text AS r FROM sift.facet_counts(tbl, 'title', q, facets, filt, true) x;
	EXECUTE replace(replace('CREATE TEMP TABLE fc_c AS SELECT row_to_json(x)::text AS r FROM (' || ref ||
		' UNION ALL SELECT NULL, ''hits'', NULL, NULL, NULL, count(*) FROM fc WHERE ' || hitwh || ') x(facet, kind, value, lo, hi, count)',
		'FROM fc ', 'FROM ' || tbl || ' '), 'FROM fc,', 'FROM ' || tbl || ',');
	SELECT count(*) INTO n FROM ((SELECT r FROM fc_a EXCEPT ALL SELECT r FROM fc_c) UNION ALL (SELECT r FROM fc_c EXCEPT ALL SELECT r FROM fc_a)) d;
	SELECT count(*) INTO m FROM ((SELECT r FROM fc_b EXCEPT ALL SELECT r FROM fc_c) UNION ALL (SELECT r FROM fc_c EXCEPT ALL SELECT r FROM fc_b)) d;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || '/' || CASE WHEN m = 0 THEN 'same' ELSE 'DIFFERENT' END || ' ' ||
		(SELECT count(*) FROM regexp_matches(plan, 'Custom Scan \(SiftAgg\)', 'g')) || ' SiftAgg, ' ||
		(SELECT count(*) FROM regexp_matches(plan, 'Search: ', 'g')) || ' searching, ' || (SELECT count(*) FROM fc_c) || ' rows';
END $$;

-- tag, query, facets, filter, oracle ($W is the where of the search and the filter), where of the hits row
CREATE TABLE fc_q (tag text PRIMARY KEY, q text, facets jsonb, filt text, ref text, hitwh text);
INSERT INTO fc_q VALUES
	('terms_text', 'wireless', '["brand"]', NULL, fc_terms('brand', 'brand', $$title @@> 'wireless'$$), $$title @@> 'wireless'$$),
	('terms_top', 'wired OR compact', '[{"expr": "category", "size": 3}]', NULL,
		fc_terms('category', 'category', $$title @@> 'wired OR compact'$$, 3), $$title @@> 'wired OR compact'$$),
	('terms_bool', 'portable', '[{"expr": "active", "missing": true}]', NULL,
		fc_terms('active', 'active', $$title @@> 'portable'$$, NULL, true), $$title @@> 'portable'$$),
	('terms_other', 'headphones', '[{"name": "b", "expr": "brand", "size": 2, "missing": true}]', NULL,
		fc_terms('b', 'brand', $$title @@> 'headphones'$$, 2, true), $$title @@> 'headphones'$$),
	('terms_filter', 'speaker OR cable', '[{"expr": "category"}]', 'price > 100 AND active',
		fc_terms('category', 'category', $$title @@> 'speaker OR cable' AND (price > 100 AND active)$$), $$title @@> 'speaker OR cable' AND (price > 100 AND active)$$),
	('no_search', NULL, '[{"expr": "brand"}]', 'qty > 50', fc_terms('brand', 'brand', 'qty > 50'), 'qty > 50'),
	('no_match', 'zzzzzz', '["brand", {"expr": "price", "edges": [100]}]', NULL,
		$$SELECT 'price', 'bucket', '[,100)', NULL, '100', 0 UNION ALL SELECT 'price', 'bucket', '[100,)', '100', NULL, 0$$, $$title @@> 'zzzzzz'$$),
	('range', 'wireless', '[{"name": "p", "expr": "price", "edges": [100, 300]}]', NULL,
		$$SELECT 'p', 'bucket', '[,100)', NULL, '100', count(*) FROM fc WHERE title @@> 'wireless' AND price < 100
		  UNION ALL SELECT 'p', 'bucket', '[100,300)', '100', '300', count(*) FROM fc WHERE title @@> 'wireless' AND price >= 100 AND price < 300
		  UNION ALL SELECT 'p', 'bucket', '[300,)', '300', NULL, count(*) FROM fc WHERE title @@> 'wireless' AND price >= 300$$, $$title @@> 'wireless'$$),
	('range_dates', 'wireless OR wired', '[{"name": "d", "expr": "added", "edges": ["2024-06-01", "2025-01-01"]}]', NULL,
		$$SELECT 'd', 'bucket', '[,2024-06-01)', NULL, '2024-06-01', count(*) FROM fc WHERE title @@> 'wireless OR wired' AND added < '2024-06-01'
		  UNION ALL SELECT 'd', 'bucket', '[2024-06-01,2025-01-01)', '2024-06-01', '2025-01-01', count(*) FROM fc WHERE title @@> 'wireless OR wired' AND added >= '2024-06-01' AND added < '2025-01-01'
		  UNION ALL SELECT 'd', 'bucket', '[2025-01-01,)', '2025-01-01', NULL, count(*) FROM fc WHERE title @@> 'wireless OR wired' AND added >= '2025-01-01'$$, $$title @@> 'wireless OR wired'$$),
	('histogram', 'classic', '[{"name": "q", "expr": "qty", "interval": 25, "offset": 5}]', NULL,
		$$SELECT 'q', 'bucket', lo::text, lo::text, (lo + 25)::text, count(*) FROM (SELECT floor((qty::numeric - 5) / 25) * 25 + 5 lo FROM fc WHERE title @@> 'classic') g WHERE lo IS NOT NULL GROUP BY lo$$, $$title @@> 'classic'$$),
	('histogram_fill', 'classic OR compact', '[{"name": "q", "expr": "qty", "interval": 3, "fill": true}]', NULL,
		$$SELECT 'q', 'bucket', (k * 3)::text, (k * 3)::text, (k * 3 + 3)::text, coalesce(c, 0)
		  FROM generate_series(0, (SELECT max(floor(qty / 3.0)) FROM fc WHERE title @@> 'classic OR compact')::int) k
		  LEFT JOIN (SELECT floor(qty / 3.0)::int k, count(*) c FROM fc WHERE title @@> 'classic OR compact' GROUP BY 1) g USING (k)$$, $$title @@> 'classic OR compact'$$),
	('histogram_float', 'compact', '[{"name": "p", "expr": "price::float8", "interval": 100.5}]', NULL,
		$$SELECT 'p', 'bucket', (k * 100.5)::text, (k * 100.5)::text, (k * 100.5 + 100.5)::text, count(*) FROM (SELECT floor(price::float8::numeric / 100.5) k FROM fc WHERE title @@> 'compact') g WHERE k IS NOT NULL GROUP BY k$$, $$title @@> 'compact'$$),
	('month', 'wireless', '[{"name": "m", "expr": "added", "unit": "month"}]', NULL,
		$$SELECT 'm', 'bucket', k::text, k::text, (k + interval '1 month')::text, count(*) FROM (SELECT date_trunc('month', added::timestamp) k FROM fc WHERE title @@> 'wireless') g GROUP BY k$$, $$title @@> 'wireless'$$),
	('week_ts', 'wired', '[{"name": "w", "expr": "ts", "unit": "week"}]', NULL,
		$$SELECT 'w', 'bucket', k::text, k::text, (k + interval '1 week')::text, count(*) FROM (SELECT date_trunc('week', ts) k FROM fc WHERE title @@> 'wired') g GROUP BY k$$, $$title @@> 'wired'$$),
	('quarter_fill', 'classic', '[{"name": "q", "expr": "added", "unit": "quarter", "fill": true}]', NULL,
		$$SELECT 'q', 'bucket', k::text, k::text, (k + interval '3 months')::text, coalesce(c, 0)
		  FROM generate_series((SELECT min(date_trunc('quarter', added::timestamp)) FROM fc WHERE title @@> 'classic'), (SELECT max(date_trunc('quarter', added::timestamp)) FROM fc WHERE title @@> 'classic'), interval '3 months') k
		  LEFT JOIN (SELECT date_trunc('quarter', added::timestamp) k, count(*) c FROM fc WHERE title @@> 'classic' GROUP BY 1) g USING (k)$$, $$title @@> 'classic'$$),
	('array', 'wireless', '[{"name": "tags", "expr": "tags", "array": true}]', NULL,
		$$SELECT 'tags', 'value', t, NULL, NULL, count(*) FROM (SELECT DISTINCT id, t FROM fc, unnest(tags) t WHERE title @@> 'wireless') g GROUP BY t$$, $$title @@> 'wireless'$$),
	('many', 'wired OR portable', '["brand", "category", {"expr": "price", "edges": [200]}, {"expr": "added", "unit": "year"}, "qty", "active"]', NULL,
		fc_terms('brand', 'brand', $$title @@> 'wired OR portable'$$) || ' UNION ALL ' || fc_terms('category', 'category', $$title @@> 'wired OR portable'$$) ||
		$$ UNION ALL SELECT 'price', 'bucket', '[,200)', NULL, '200', count(*) FROM fc WHERE title @@> 'wired OR portable' AND price < 200
		   UNION ALL SELECT 'price', 'bucket', '[200,)', '200', NULL, count(*) FROM fc WHERE title @@> 'wired OR portable' AND price >= 200
		   UNION ALL SELECT 'added', 'bucket', k::text, k::text, (k + interval '1 year')::text, count(*) FROM (SELECT date_trunc('year', added::timestamp) k FROM fc WHERE title @@> 'wired OR portable') g GROUP BY k$$ ||
		' UNION ALL ' || fc_terms('qty', 'qty', $$title @@> 'wired OR portable'$$) || ' UNION ALL ' || fc_terms('active', 'active', $$title @@> 'wired OR portable'$$),
		$$title @@> 'wired OR portable'$$),
	-- a facet leaves its own selection out of its counts and applies the selections of the others
	('drill', 'wireless OR wired', $$[{"name": "brand", "expr": "brand", "selected": ["acme", "zeta"]},
		{"name": "p", "expr": "price", "edges": [100, 300], "selected": [1]}, {"name": "category", "expr": "category"}]$$, 'active',
		fc_terms('brand', 'brand', $$title @@> 'wireless OR wired' AND (active) AND price >= 100 AND price < 300$$) ||
		$$ UNION ALL SELECT 'p', 'bucket', '[,100)', NULL, '100', count(*) FROM fc WHERE title @@> 'wireless OR wired' AND (active) AND brand IN ('acme', 'zeta') AND price < 100
		   UNION ALL SELECT 'p', 'bucket', '[100,300)', '100', '300', count(*) FROM fc WHERE title @@> 'wireless OR wired' AND (active) AND brand IN ('acme', 'zeta') AND price >= 100 AND price < 300
		   UNION ALL SELECT 'p', 'bucket', '[300,)', '300', NULL, count(*) FROM fc WHERE title @@> 'wireless OR wired' AND (active) AND brand IN ('acme', 'zeta') AND price >= 300 UNION ALL $$ ||
		fc_terms('category', 'category', $$title @@> 'wireless OR wired' AND (active) AND brand IN ('acme', 'zeta') AND price >= 100 AND price < 300$$),
		$$title @@> 'wireless OR wired' AND (active) AND brand IN ('acme', 'zeta') AND price >= 100 AND price < 300$$),
	('drill_buckets', 'wireless', $$[{"name": "q", "expr": "qty", "interval": 25, "selected": [25, 50]}, {"name": "m", "expr": "added", "unit": "month", "selected": ["2024-03-01 00:00:00"]},
		{"name": "t", "expr": "tags", "array": true, "selected": ["red"]}, {"name": "category", "expr": "category", "filter": "category > 2"}]$$, NULL,
		$$SELECT 'q', 'bucket', lo::text, lo::text, (lo + 25)::text, count(*) FROM (SELECT floor(qty::numeric / 25) * 25 lo FROM fc
			WHERE title @@> 'wireless' AND date_trunc('month', added::timestamp) = '2024-03-01' AND tags && ARRAY['red'] AND category > 2) g WHERE lo IS NOT NULL GROUP BY lo
		  UNION ALL SELECT 'm', 'bucket', k::text, k::text, (k + interval '1 month')::text, count(*) FROM (SELECT date_trunc('month', added::timestamp) k FROM fc
			WHERE title @@> 'wireless' AND ((qty >= 25 AND qty < 50) OR (qty >= 50 AND qty < 75)) AND tags && ARRAY['red'] AND category > 2) g GROUP BY k
		  UNION ALL SELECT 't', 'value', e, NULL, NULL, count(*) FROM (SELECT DISTINCT id, e FROM fc, unnest(tags) e
			WHERE title @@> 'wireless' AND ((qty >= 25 AND qty < 50) OR (qty >= 50 AND qty < 75)) AND date_trunc('month', added::timestamp) = '2024-03-01' AND category > 2) g GROUP BY e
		  UNION ALL $$ || fc_terms('category', 'category', $$title @@> 'wireless' AND ((qty >= 25 AND qty < 50) OR (qty >= 50 AND qty < 75)) AND date_trunc('month', added::timestamp) = '2024-03-01' AND tags && ARRAY['red']$$),
		$$title @@> 'wireless' AND ((qty >= 25 AND qty < 50) OR (qty >= 50 AND qty < 75)) AND date_trunc('month', added::timestamp) = '2024-03-01' AND tags && ARRAY['red'] AND category > 2$$);

INSERT INTO fc_q VALUES
	('drill_dates', 'wireless', $$[{"name": "q", "expr": "qty", "interval": 25, "selected": [25, 50]}, {"name": "m", "expr": "added", "unit": "month", "selected": ["2024-03-01 00:00:00"]},
		{"name": "category", "expr": "category"}]$$, NULL,
		$$SELECT 'q', 'bucket', lo::text, lo::text, (lo + 25)::text, count(*) FROM (SELECT floor(qty::numeric / 25) * 25 lo FROM fc
			WHERE title @@> 'wireless' AND date_trunc('month', added::timestamp) = '2024-03-01') g WHERE lo IS NOT NULL GROUP BY lo
		  UNION ALL SELECT 'm', 'bucket', k::text, k::text, (k + interval '1 month')::text, count(*) FROM (SELECT date_trunc('month', added::timestamp) k FROM fc
			WHERE title @@> 'wireless' AND ((qty >= 25 AND qty < 50) OR (qty >= 50 AND qty < 75))) g GROUP BY k
		  UNION ALL $$ || fc_terms('category', 'category', $$title @@> 'wireless' AND ((qty >= 25 AND qty < 50) OR (qty >= 50 AND qty < 75)) AND date_trunc('month', added::timestamp) = '2024-03-01'$$),
		$$title @@> 'wireless' AND ((qty >= 25 AND qty < 50) OR (qty >= 50 AND qty < 75)) AND date_trunc('month', added::timestamp) = '2024-03-01'$$);

INSERT INTO fc_q VALUES
	('range_missing', 'wireless OR wired', '[{"name": "p", "expr": "price", "edges": [100], "missing": true}, {"name": "q", "expr": "qty", "edges": [20, 60], "missing": true}]', NULL,
		$$SELECT 'p', 'bucket', '[,100)', NULL, '100', count(*) FROM fc WHERE title @@> 'wireless OR wired' AND price < 100
		  UNION ALL SELECT 'p', 'bucket', '[100,)', '100', NULL, count(*) FROM fc WHERE title @@> 'wireless OR wired' AND price >= 100
		  UNION ALL SELECT 'p', 'missing', NULL, NULL, NULL, count(*) FROM fc WHERE title @@> 'wireless OR wired' AND price IS NULL HAVING count(*) > 0
		  UNION ALL SELECT 'q', 'bucket', '[,20)', NULL, '20', count(*) FROM fc WHERE title @@> 'wireless OR wired' AND qty < 20
		  UNION ALL SELECT 'q', 'bucket', '[20,60)', '20', '60', count(*) FROM fc WHERE title @@> 'wireless OR wired' AND qty >= 20 AND qty < 60
		  UNION ALL SELECT 'q', 'bucket', '[60,)', '60', NULL, count(*) FROM fc WHERE title @@> 'wireless OR wired' AND qty >= 60
		  UNION ALL SELECT 'q', 'missing', NULL, NULL, NULL, count(*) FROM fc WHERE title @@> 'wireless OR wired' AND qty IS NULL HAVING count(*) > 0$$, $$title @@> 'wireless OR wired'$$);

CREATE FUNCTION fc_stage(name text, tbl regclass DEFAULT 'fc') RETURNS TABLE (stage text, results text) LANGUAGE sql AS $$
	SELECT name, string_agg(tag || '=' || fc_check(tbl, q, facets, filt, ref, hitwh), E'\n' ORDER BY tag) FROM fc_q $$;

SELECT * FROM fc_stage('build');

-- the order of the rows, the other row and the labels
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"name": "c", "expr": "category", "size": 3, "order": "value desc", "missing": true},
	{"expr": "price", "type": "range", "edges": [100, 300]}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"name": "c", "expr": "category", "size": 2, "order": "count asc"}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"name": "c", "expr": "category", "size": 2, "order": "value"}]', 'qty < 20');

-- the scans of the statement: one per facet (the facets that count the same rows are not merged), the ranges with one WHERE share one
SELECT (SELECT count(*) FROM regexp_matches(sift.facet_sql('fc', 'title', 'w1', '["brand", "category", "qty", "active", "added", "price"]'), ' MATERIALIZED ', 'g')) AS scans_of_six,
	   (SELECT count(*) FROM regexp_matches(sift.facet_sql('fc', 'title', 'w1', '["brand", {"expr": "price", "edges": [100]}, {"expr": "qty", "edges": [5, 9]}]'), ' MATERIALIZED ', 'g')) AS scans_with_ranges,
	   (SELECT count(*) FROM regexp_matches(sift.facet_sql('fc', 'title', 'w1', '[{"expr": "brand", "selected": ["a"]}, {"expr": "price", "edges": [100], "selected": [0]}, {"expr": "qty", "edges": [5]}]'), ' MATERIALIZED ', 'g')) AS scans_drill;

-- the same facets over a table without a column index (the planner's plan) and over a sift_table (its own column scan)
CREATE TABLE fcn (LIKE fc INCLUDING DEFAULTS);
ALTER TABLE fcn ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO fcn SELECT * FROM fc;
CREATE INDEX fcn_bm ON fcn USING sift (title);
CREATE TABLE fct (LIKE fc INCLUDING DEFAULTS) USING sift_table;
INSERT INTO fct SELECT * FROM fc;
CREATE INDEX fct_bm ON fct USING sift (title);
VACUUM (ANALYZE) fcn, fct;
SELECT * FROM fc_stage('no column index', 'fcn');
SELECT * FROM fc_stage('sift_table', 'fct');

-- what the callers get wrong
\set VERBOSITY terse
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '{"expr": "brand"}');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"type": "nope", "expr": "brand"}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"type": "histogram", "expr": "qty"}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"type": "range", "expr": "price", "edges": []}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"expr": "brand", "order": "random"}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"expr": "added", "unit": "fortnight"}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"expr": "brand", "type": "histogram", "interval": 5, "array": true}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"size": 3}]');
SELECT * FROM sift.facet_counts('fc', NULL, 'wireless', '["brand"]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '["nope"]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"expr": "qty", "interval": 0.0001, "fill": true}]');
SELECT * FROM sift.facet_counts('fc', 'title', 'wireless', '[{"expr": "ts", "unit": "second", "fill": true}]');

-- a role that can read the table gets the counts of what it can read (the statement runs as the caller)
CREATE ROLE fc_reader;
GRANT SELECT ON fc TO fc_reader;
GRANT USAGE ON SCHEMA sift TO fc_reader;
SET ROLE fc_reader;
SELECT sum(count) FROM sift.facet_counts('fc', 'title', 'wireless', '["category"]');
RESET ROLE;
CREATE ROLE fc_nobody;
GRANT USAGE ON SCHEMA sift TO fc_nobody;
SET ROLE fc_nobody;
SELECT sum(count) FROM sift.facet_counts('fc', 'title', 'wireless', '["category"]');
RESET ROLE;
DROP OWNED BY fc_reader, fc_nobody;
DROP ROLE fc_reader, fc_nobody;
\set VERBOSITY default

-- writes: each kind of change, with the facets of the table compared again
SET sift.test_search_chunk_sbs = 1;
SELECT * FROM fc_stage('chunks of one superblock');
RESET sift.test_search_chunk_sbs;

INSERT INTO fc (title, brand, category, price, qty, added, ts, active, tags)
SELECT 'wireless fresh w' || i, (ARRAY['acme', 'new', NULL])[1 + i % 3], 20 + i % 3, i % 700, i % 40, date '2025-06-01' + i % 90, timestamp '2025-06-01' + i % 90 * interval '1 day',
	   i % 2 = 0, ARRAY['red', 'fresh']
FROM generate_series(1, 900) i;
SELECT * FROM fc_stage('insert');

UPDATE fc SET brand = 'zeta', price = price + 150, added = added + 40, ts = ts + interval '40 days', qty = qty + 30, tags = tags || 'moved'::text WHERE id % 7 = 0;
SELECT * FROM fc_stage('update');

DELETE FROM fc WHERE id % 5 = 0;
SELECT * FROM fc_stage('delete');

BEGIN;
DELETE FROM fc WHERE id % 3 = 0;
INSERT INTO fc (title, brand, category, price, qty, added, ts, active, tags) SELECT 'wireless own w' || i, 'own', 50, 99, 1, date '2024-02-02', timestamp '2024-02-02', true, ARRAY['own'] FROM generate_series(1, 50) i;
SELECT * FROM fc_stage('own transaction');
ROLLBACK;
SELECT * FROM fc_stage('rollback');

VACUUM fc;
SELECT * FROM fc_stage('vacuum');
SELECT sift.promote('fc_bm') >= 0 AS promoted_bm, sift.col_promote('fc_col') >= 0 AS promoted_col;
SELECT * FROM fc_stage('promote');
SELECT sift.merge('fc_bm', true) >= 0 AS merged_bm, sift.col_merge('fc_col', true) >= 0 AS merged_col;
SELECT * FROM fc_stage('merge');

SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT * FROM fc_stage('parallel');
