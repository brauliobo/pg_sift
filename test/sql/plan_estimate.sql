-- The planner asks the index for the same estimate several times per statement (the selectivity of the clause,
-- the cost of each path, the index path), and the index remembers the last answers. An answer holds while the
-- directory of the index is the one it was computed from: a promotion, a merge or a new file (REINDEX) and a
-- different query are estimated again
CREATE EXTENSION pg_sift;

CREATE FUNCTION plan_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (jsonb_path_query_first(j, 'strict $.** ? (@."Node Type" == "Custom Scan")') ->> 'Plan Rows')::bigint;
END $$;

CREATE TABLE em (id int, body text);
CREATE INDEX em_body ON em USING sift (body);
INSERT INTO em SELECT i, 'alpha beta' FROM generate_series(1, 100) i;

-- the estimate counts the promoted documents: promoting between two plans of the same query changes the
-- directory, and the second plan must not reuse the first one's answer
BEGIN;
SELECT plan_rows('SELECT id FROM em WHERE body @@> ''alpha''') AS before_promote;
INSERT INTO em SELECT i, 'alpha gamma' FROM generate_series(101, 1100) i;
SELECT sift.promote('em_body');
SELECT plan_rows('SELECT id FROM em WHERE body @@> ''alpha''') > 1000 AS after_promote;
SELECT plan_rows('SELECT id FROM em WHERE body @@> ''gamma''') > 900 AS other_query;
ROLLBACK;

-- the same with no write between the two plans
CREATE FUNCTION promote_between() RETURNS TABLE (before_rows bigint, after_rows bigint) LANGUAGE plpgsql AS $$
BEGIN
	before_rows := plan_rows('SELECT id FROM em WHERE body @@> ''beta''');
	PERFORM sift.promote('em_body');
	after_rows := plan_rows('SELECT id FROM em WHERE body @@> ''beta''');
	RETURN NEXT;
END $$;
INSERT INTO em SELECT i, 'beta' FROM generate_series(2000, 2899) i;
SELECT before_rows, after_rows > 900 AS after_promote FROM promote_between();

-- a new index file: the same query is estimated again from the rebuilt index
SELECT plan_rows('SELECT id FROM em WHERE body @@> ''beta''') > 900 AS before_reindex;
DELETE FROM em WHERE body @@> 'beta';
REINDEX INDEX em_body;
SELECT plan_rows('SELECT id FROM em WHERE body @@> ''beta''') < 100 AS after_reindex;

-- deleted documents count until VACUUM rewrites their segment: the estimate follows the directory, not the heap
INSERT INTO em SELECT i, 'delta' FROM generate_series(5000, 7000) i;
SELECT sift.promote('em_body');
SELECT plan_rows('SELECT id FROM em WHERE body @@> ''delta''') > 1500 AS before_delete;
DELETE FROM em WHERE body @@> 'delta' AND id > 5100;
SELECT plan_rows('SELECT id FROM em WHERE body @@> ''delta''') > 1500 AS after_delete;
VACUUM em;
SELECT plan_rows('SELECT id FROM em WHERE body @@> ''delta''') < 300 AS after_vacuum;

DROP FUNCTION promote_between(), plan_rows(text);
DROP TABLE em;
DROP EXTENSION pg_sift;
