-- SiftHybridScan and SiftHybridAgg with partial indexes: a text index whose predicate the query does not imply holds only some of the rows and is not the match of the scan, and the
-- quals of the OR form keep a conjunct the predicate of the store implies, since the matches of the text index the store does not hold are read past it. Every result is compared with the
-- same query that the hybrid node refuses (a volatile qual is added), and the node is read from EXPLAIN.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
CREATE TABLE pp_text (id int PRIMARY KEY, tenant int NOT NULL, category int NOT NULL, body text NOT NULL);
INSERT INTO pp_text SELECT i, i % 3, i % 20, concat_ws(' ', CASE WHEN i % 500 = 0 THEN 'rareterm' END, 'tok' || i % 11) FROM generate_series(1, 20000) i;
CREATE INDEX pp_text_body ON pp_text USING sift (body) WHERE tenant = 1;
CREATE INDEX pp_text_col ON pp_text USING sift_col (tenant, id, category);
ANALYZE pp_text;
VACUUM (FREEZE) pp_text;
CREATE TABLE pp_col (id int PRIMARY KEY, status int NOT NULL, category int NOT NULL, body text NOT NULL);
INSERT INTO pp_col SELECT i, i % 3, i % 20, concat_ws(' ', CASE WHEN i % 500 = 0 THEN 'rareterm' END, 'tok' || i % 11) FROM generate_series(1, 20000) i;
CREATE INDEX pp_col_body ON pp_col USING sift (body);
CREATE INDEX pp_col_col ON pp_col USING sift_col (status, id, category) WHERE status = 2;
ANALYZE pp_col;
VACUUM (FREEZE) pp_col;
RESET client_min_messages;
SET effective_cache_size = '8kB';
SET enable_seqscan = off;
SET enable_bitmapscan = off;

CREATE FUNCTION pp_node(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; n text := 'none';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftHybrid' THEN n := substring(l from 'Custom Scan \((SiftHybrid[A-Za-z]+)\)'); END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION pp_cmp(q text, OUT node text, OUT rows bigint, OUT diff bigint) LANGUAGE plpgsql AS $$
BEGIN
	node := pp_node(q);
	EXECUTE 'CREATE TEMP TABLE pp_a AS ' || q;
	EXECUTE 'CREATE TEMP TABLE pp_b AS ' || regexp_replace(q, 'WHERE ', 'WHERE random() < 2 AND ');
	SELECT count(*) INTO rows FROM pp_b;
	SELECT count(*) INTO diff FROM ((TABLE pp_a EXCEPT ALL TABLE pp_b) UNION ALL (TABLE pp_b EXCEPT ALL TABLE pp_a)) d;
	DROP TABLE pp_a, pp_b;
END $$;

-- the text index holds the rows of tenant 1 only: a match without that qual is not read from it
SELECT q, c.* FROM (VALUES
	('SELECT id, tenant FROM pp_text WHERE body @@> ''rareterm'' AND category = 0'),
	('SELECT count(*) FROM pp_text WHERE body @@> ''rareterm'' AND category = 0'),
	('SELECT id, tenant FROM pp_text WHERE (body @@> ''rareterm'' OR category = 3)'),
	('SELECT id, tenant FROM pp_text WHERE body @@> ''rareterm'' AND category = 0 AND tenant = 1')
) v(q), pp_cmp(q) c;

-- the store holds the rows of status 2 only: the matches the text index adds still pass status = 2
SELECT q, c.* FROM (VALUES
	('SELECT id, status FROM pp_col WHERE (body @@> ''rareterm'' OR category = 1) AND status = 2'),
	('SELECT id, status FROM pp_col WHERE body @@> ''rareterm'' AND category < 10 AND status = 2')
) v(q), pp_cmp(q) c;
DROP TABLE pp_text, pp_col;
DROP FUNCTION pp_node(text), pp_cmp(text);
DROP EXTENSION pg_sift;
