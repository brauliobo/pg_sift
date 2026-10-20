-- Facet counts against four GROUP BYs, and field weights against one field (steps/extras.sh fields), on the orders of bench/sql/store/gen.sql.
--   psql -f fields.sql          Output: lines `ROW case arm metric value note`, tab separated.
\set ON_ERROR_STOP on
\pset format unaligned
\pset tuples_only on
\pset fieldsep '\t'
\ir lib.sql
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TABLE orders AS SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM orders_src ORDER BY id;
ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, quantity, created_at, (date_trunc('month', created_at)::date));
CREATE INDEX orders_title ON orders USING sift (title);
CREATE INDEX orders_fields ON orders USING sift (sift.sections(title, category)) WITH (fields = 'title,category');
VACUUM (FREEZE, ANALYZE) orders;

-- four facets of one search: top 10 categories, top 10 countries, two price ranges, the month
SELECT 'ROW', 'index', 'orders_col', 'bytes', pg_relation_size('orders_col'), '';
SELECT 'ROW', 'index', 'orders_title', 'bytes', pg_relation_size('orders_title'), '';
SELECT 'ROW', 'index', 'orders_fields', 'bytes', pg_relation_size('orders_fields'), '';
CREATE TEMP TABLE searches(label text, q text);
INSERT INTO searches VALUES ('wireless', 'wireless'), ('wireless OR portable OR compact OR premium', 'wireless OR portable OR compact OR premium'), ('wireless smart black (selective)', 'wireless smart black');
SELECT 'ROW', 'facets: ' || label, arm, 'ms', timed_ms(sql, 21, settings), n_hits
FROM (SELECT label, q, (SELECT count(*) FROM orders WHERE title @@> q) AS n_hits FROM searches) s
CROSS JOIN LATERAL (VALUES
  ('facet_counts', format($f$SELECT * FROM sift.facet_counts('orders', 'title', %L, '[{"expr":"category","size":10},{"expr":"country","size":10},{"expr":"amount","edges":[100,300]},{"expr":"created_at","unit":"month"}]')$f$, q), ''),
  ('four GROUP BYs, SiftAgg', format($f$SELECT 1 FROM (SELECT category, count(*) FROM orders WHERE title @@> %1$L GROUP BY 1 ORDER BY 2 DESC LIMIT 10) a UNION ALL SELECT 2 FROM (SELECT country, count(*) FROM orders WHERE title @@> %1$L GROUP BY 1 ORDER BY 2 DESC LIMIT 10) b UNION ALL SELECT 3 FROM (SELECT count(*) FILTER (WHERE amount < 100), count(*) FILTER (WHERE amount >= 100 AND amount < 300), count(*) FILTER (WHERE amount >= 300) FROM orders WHERE title @@> %1$L) c UNION ALL SELECT 4 FROM (SELECT date_trunc('month', created_at), count(*) FROM orders WHERE title @@> %1$L GROUP BY 1) d$f$, q), 'sift.enable_agg=on'),
  ('four GROUP BYs, normal plan', format($f$SELECT 1 FROM (SELECT category, count(*) FROM orders WHERE title @@> %1$L GROUP BY 1 ORDER BY 2 DESC LIMIT 10) a UNION ALL SELECT 2 FROM (SELECT country, count(*) FROM orders WHERE title @@> %1$L GROUP BY 1 ORDER BY 2 DESC LIMIT 10) b UNION ALL SELECT 3 FROM (SELECT count(*) FILTER (WHERE amount < 100), count(*) FILTER (WHERE amount >= 100 AND amount < 300), count(*) FILTER (WHERE amount >= 300) FROM orders WHERE title @@> %1$L) c UNION ALL SELECT 4 FROM (SELECT date_trunc('month', created_at), count(*) FROM orders WHERE title @@> %1$L GROUP BY 1) d$f$, q), 'sift.enable_agg=off')
) a(arm, sql, settings);

-- the top 10 of a two-word search: one field, two fields as one text with equal weights, and weighted
SELECT 'ROW', 'fields: wireless headphones', arm, 'ms', timed_ms(sql, 21, 'enable_seqscan=off'), ''
FROM (VALUES
  ('title only', 'SELECT id FROM orders WHERE title @@> ''wireless headphones'' ORDER BY sift.score(ctid) DESC LIMIT 10'),
  ('title, category (weights 1, 1)', 'SELECT id FROM orders WHERE sift.sections(title, category) @@> sift.field_query(''wireless headphones'', ''title, category'') ORDER BY sift.score(ctid) DESC LIMIT 10'),
  ('title^3, category', 'SELECT id FROM orders WHERE sift.sections(title, category) @@> sift.field_query(''wireless headphones'', ''title^3, category'') ORDER BY sift.score(ctid) DESC LIMIT 10')
) a(arm, sql);
