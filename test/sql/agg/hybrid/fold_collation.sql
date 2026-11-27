-- S119-13: the hybrid node decides lower(col) and ILIKE from the stored bytes only under a collation whose lower() folds the ASCII capitals and no other character. The builtin
-- C.UTF-8 (pg_c_utf8) orders by bytes but lowers the Kelvin sign to k: every answer is the one of the same query that the node leaves to the heap (a volatile qual is added).
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
CREATE TABLE fc_docs (id int PRIMARY KEY, tenant int NOT NULL, label text COLLATE pg_c_utf8 NOT NULL, body text NOT NULL);
INSERT INTO fc_docs SELECT i, i % 300, CASE (i / 2) % 4 WHEN 0 THEN U&'\212A' WHEN 1 THEN 'K' WHEN 2 THEN U&'\212Aey' ELSE 'key' || i END,
	concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, 'tok' || i % 13) FROM generate_series(1, 4000) i;
CREATE INDEX fc_text ON fc_docs USING sift (body);
CREATE INDEX fc_col ON fc_docs USING sift_col (id, tenant, label);
ANALYZE fc_docs;
VACUUM fc_docs;
RESET client_min_messages;
SET enable_nestloop = off;
SET effective_cache_size = '8kB';
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION fc_node(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftHybrid' THEN RETURN substring(l from 'Custom Scan \((SiftHybrid[A-Za-z]+)\)'); END IF;
	END LOOP;
	RETURN 'none';
END $$;
CREATE FUNCTION fc_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(x::text, '';'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

SELECT name, fc_node(q) AS node, fc_rows(q) = fc_rows(regexp_replace(q, 'WHERE ', 'WHERE random() < 2 AND ')) AS same_rows
FROM (VALUES
	('ilike_eq',	$q$SELECT count(*), sum(id) FROM fc_docs WHERE label ILIKE 'k' AND tenant < 300 AND body @@> 'common'$q$),
	('ilike_prefix',$q$SELECT count(*), sum(id) FROM fc_docs WHERE label ILIKE 'ke%' AND tenant < 300 AND body @@> 'common'$q$),
	('lower_eq',	$q$SELECT count(*), sum(id) FROM fc_docs WHERE lower(label) = 'k' AND tenant < 300 AND body @@> 'common'$q$),
	('lower_like',	$q$SELECT count(*), sum(id) FROM fc_docs WHERE lower(label) LIKE 'ke%' AND tenant < 300 AND body @@> 'common'$q$),
	('c_ilike',		$q$SELECT count(*), sum(id) FROM fc_docs WHERE label COLLATE "C" ILIKE 'k' AND tenant < 300 AND body @@> 'common'$q$),
	('c_lower',		$q$SELECT count(*), sum(id) FROM fc_docs WHERE lower(label COLLATE "C") LIKE 'ke%' AND tenant < 300 AND body @@> 'common'$q$)) v(name, q)
ORDER BY name;

DROP FUNCTION fc_node(text), fc_rows(text);
DROP TABLE fc_docs;
DROP EXTENSION pg_sift;
