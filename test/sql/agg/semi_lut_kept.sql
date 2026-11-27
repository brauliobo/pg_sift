-- A semi, anti or inner join by an integer key is a lookup of SiftAggV6 that loads the rows of the joined table its condition keeps. Each statement runs twice in a
-- row, after the rows of the table change, and with a key that repeats in the table (an inner join repeats a tuple, a semi join does not): the rows must be those
-- of the normal plan, and the lookup loads the rows of the table the statement sees.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int, dim_a_id int, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + (i::bigint * 7919) % 3000, i % 100 FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, amount);
CREATE TABLE dim_b (group_id int, dim_a_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO dim_b SELECT 1, 1 + i * 3, NULL FROM generate_series(0, 999) i;
INSERT INTO dim_b SELECT 2, 2 + i * 5, CASE WHEN i % 4 = 0 THEN now() END FROM generate_series(0, 599) i;
VACUUM (FREEZE, ANALYZE) ev, dim_b;

CREATE FUNCTION run(q text, push bool) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg(q, push) $$;
CREATE FUNCTION lookup_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; node text := 'normal'; loaded text := '0';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \((SiftAgg\w*)\)' THEN node := substring(l from 'Custom Scan \((SiftAgg\w*)\)'); END IF;
		IF l ~ '(\d+) rows loaded' THEN loaded := substring(l from '(\d+) rows loaded'); END IF;
	END LOOP;
	RETURN format('%s, %s rows loaded', node, loaded);
END $$;
CREATE TABLE q (n int PRIMARY KEY, shape text, sql text);
INSERT INTO q VALUES
	(1, 'inner, group 1', 'SELECT count(*), sum(amount) FROM ev e JOIN dim_b m ON m.dim_a_id = e.dim_a_id AND m.group_id = 1'),
	(2, 'semi, group 1', 'SELECT count(*), sum(amount) FROM ev e WHERE e.dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 1)'),
	(3, 'anti, group 1', 'SELECT count(*), sum(amount) FROM ev e WHERE NOT EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = e.dim_a_id AND m.group_id = 1)'),
	(4, 'inner, group 2 live', 'SELECT count(*), sum(amount) FROM ev e JOIN dim_b m ON m.dim_a_id = e.dim_a_id AND m.group_id = 2 AND m.deleted_at IS NULL'),
	(5, 'inner, group 1 after the others', 'SELECT count(*), sum(amount) FROM ev e JOIN dim_b m ON m.dim_a_id = e.dim_a_id AND m.group_id = 1');

-- the statements of the table: twice in a row, the answer of the pushed plan against the normal plan's, and the rows the lookup loaded
CREATE FUNCTION check_all(which text) RETURNS TABLE (stage text, n int, shape text, round1 bool, round2 bool, lookup text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT check_all.which, q.n, q.shape, run(q.sql, true) = run(q.sql, false), run(q.sql, true) = run(q.sql, false), lookup_of(q.sql) FROM q ORDER BY q.n;
END $$;
SELECT * FROM check_all('built');

-- rows deleted and added
DELETE FROM dim_b WHERE group_id = 1 AND dim_a_id < 200;
INSERT INTO dim_b SELECT 1, 10000 + i, NULL FROM generate_series(1, 50) i;
UPDATE dim_b SET deleted_at = NULL WHERE group_id = 2 AND dim_a_id < 100;
VACUUM dim_b;
SELECT * FROM check_all('changed');

-- a key that repeats: the inner join repeats a tuple per row of its key, the semi join does not
INSERT INTO dim_b VALUES (1, 301, NULL), (1, 301, NULL);
VACUUM dim_b;
SELECT * FROM check_all('repeated');
DELETE FROM dim_b WHERE group_id = 1 AND dim_a_id = 301;
VACUUM dim_b;
SELECT * FROM check_all('unique again');

DROP FUNCTION check_all(text), run(text, bool), lookup_of(text);
DROP TABLE q, ev, dim_b;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
