-- SiftAggV6Push answers the join of a fact and a small relation with the values of one column pair as the key of the scan, so it takes a join only when that equality is the whole join: any other clause that
-- holds a column of each side (an equality with an expression of the outer row, a second column, an inequality, an OR, a second correlation of the sub-select) stays with PostgreSQL's join. Every query is
-- run with the node and without it (sift.enable_agg): the answers are the same rows, and not empty.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;
SET jit = off;

CREATE TABLE ev (id int PRIMARY KEY, dim_id int NOT NULL, small_id int2 NOT NULL, kind text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + (i::bigint * 7919) % 300, (i % 50)::int2, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4] FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, dim_id, small_id, kind);
CREATE TABLE dims (id int PRIMARY KEY, country int NOT NULL, tier int2 NOT NULL);
INSERT INTO dims SELECT i, i % 7, (i % 5)::int2 FROM generate_series(1, 300) i;
CREATE TABLE pairs (dim_id int NOT NULL, small_id int NOT NULL);
INSERT INTO pairs SELECT dim_id, small_id FROM ev WHERE id % 97 = 0;
VACUUM (ANALYZE) ev;
ANALYZE dims;
ANALYZE pairs;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(r::text, E''\n'' ORDER BY r::text)) FROM (%s) r', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	a := answer(q);
	PERFORM set_config('sift.enable_agg', 'off', true);
	RETURN a IS NOT NULL AND a = answer(q);
END $$;

SELECT shape, same(q) AS same FROM (VALUES
	('semi, one key',                   'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id) GROUP BY kind'),
	('semi, IN',                        'SELECT kind, count(*) FROM ev WHERE ev.dim_id IN (SELECT d.id FROM dims d) GROUP BY kind'),
	('semi, filter on the small side',  'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.tier = 1) GROUP BY kind'),
	('inner, unique small side',        'SELECT kind, count(*) FROM ev JOIN dims d ON d.id = ev.dim_id GROUP BY kind'),
	('semi, expression of the outer',   'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.tier = ev.small_id % 5) GROUP BY kind'),
	('IN, expression of the outer',     'SELECT kind, count(*) FROM ev WHERE ev.dim_id IN (SELECT d.id FROM dims d WHERE d.tier = ev.small_id % 5) GROUP BY kind'),
	('semi, inequality',                'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.tier < ev.small_id) GROUP BY kind'),
	('IN, inequality',                  'SELECT kind, count(*) FROM ev WHERE ev.dim_id IN (SELECT d.id FROM dims d WHERE d.tier < ev.small_id) GROUP BY kind'),
	('semi, OR',                        'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND (d.tier = ev.small_id OR d.country = ev.small_id)) GROUP BY kind'),
	('semi, second correlation',        'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.country = ev.small_id) GROUP BY kind'),
	('IN, second correlation',          'SELECT kind, count(*) FROM ev WHERE ev.dim_id IN (SELECT d.id FROM dims d WHERE d.country = ev.small_id) GROUP BY kind'),
	('semi, two keys',                  'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM pairs g WHERE g.dim_id = ev.dim_id AND g.small_id = ev.small_id) GROUP BY kind'),
	('semi, a class of three columns',  'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.id = ev.id % 300 + 1) GROUP BY kind'),
	('semi, expression pinned',         'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.tier = ev.small_id % 5 AND d.tier = 3) GROUP BY kind'),
	('semi, two classes',               'SELECT kind, count(*) FROM ev WHERE EXISTS (SELECT 1 FROM dims d WHERE d.id = ev.dim_id AND d.country = ev.small_id::int) GROUP BY kind'),
	('inner, expression of the outer',  'SELECT kind, count(*) FROM ev JOIN dims d ON d.id = ev.dim_id AND d.tier = ev.small_id % 5 GROUP BY kind'),
	('inner, inequality',               'SELECT kind, count(*) FROM ev JOIN dims d ON d.id = ev.dim_id AND d.tier < ev.small_id GROUP BY kind')
) AS t(shape, q);

DROP TABLE ev, dims, pairs;
DROP FUNCTION answer(text), same(text);
DROP EXTENSION pg_sift;
