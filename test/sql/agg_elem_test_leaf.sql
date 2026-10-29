-- A containment of an array of objects with two keys, and an EXISTS over its elements with two tests, are an element predicate whose child is an AND: it has no stored leaf
-- (leaf -1), and the element test of the scan must decline it before it looks up the leaf of the layout (a read before the array of leaves, found by the sanitizer lane).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE eo (id int, tenant_id int, items jsonb) WITH (autovacuum_enabled = off);
INSERT INTO eo SELECT g, g % 10, (SELECT jsonb_agg(jsonb_build_object('category', (ARRAY['Electronics', 'Books', 'Toys'])[1 + (g + i) % 3],
	'kind', (ARRAY['express', 'standard'])[1 + (g * i) % 2], 'price', (g + i) % 50)) FROM generate_series(1, 1 + g % 4) i) FROM generate_series(1, 3000) g;
VACUUM (FREEZE, ANALYZE) eo;
CREATE INDEX eo_col ON eo USING sift_col (id, tenant_id, items) WITH (paths = 'items[]{category text, kind text, price float8}');

CREATE FUNCTION eo_rows(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', agg::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

SELECT n, eo_rows(q, true) IS NOT DISTINCT FROM eo_rows(q, false) AS same, eo_rows(q, true) <> '' AS not_empty
FROM (VALUES
	(1, 'SELECT tenant_id, count(*) FROM eo WHERE items @> ''[{"category": "Electronics", "kind": "express"}]'' GROUP BY 1'),
	(2, 'SELECT tenant_id, count(*) FROM eo WHERE items @> ''[{"category": "Books"}]'' GROUP BY 1'),
	(3, 'SELECT tenant_id, count(*) FROM eo WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(items) AS o(category text, kind text) WHERE o.category = ''Toys'' AND o.kind = ''standard'') GROUP BY 1'),
	(4, 'SELECT tenant_id, count(*) FROM eo WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(items) AS o(category text, price float8) WHERE o.category = ''Toys'' AND o.price > 20) GROUP BY 1')
) v(n, q) ORDER BY n;

DROP TABLE eo;
DROP FUNCTION eo_rows(text, bool);
DROP EXTENSION pg_sift;
