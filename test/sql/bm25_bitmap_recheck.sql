-- A match on a key whose index stems (stemmer = 'english') is the call sift.match_idx(key, query, index) in the plan. A bitmap heap scan over that index and
-- another one (a btree on a column) gave the rows of the bitmap exactly and still analyzed the document of every row it fetched again, as a filter: the call is
-- a recheck condition, which only the rows of a lossy page go through. The rows (and so the scores) are those of the filter.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.enable_scan = off;
SET sift.enable_count = off;
SET enable_seqscan = off;
SET enable_indexscan = off;

CREATE TABLE bm (id int PRIMARY KEY, tenant int, body text);
INSERT INTO bm SELECT i, i % 40,
	(ARRAY['running quickly', 'the runner runs', 'a fast run', 'walking slowly', 'jumped and jumping', 'nothing here', 'runs and running', 'jumps high'])[1 + i % 8]
	|| ' ' || (ARRAY['dogs', 'a dog', 'cats', 'the cat'])[1 + (i / 8) % 4] || ' ' || i
FROM generate_series(1, 6000) i;
CREATE INDEX bm_body ON bm USING sift (body) WITH (stemmer = 'english');
CREATE INDEX bm_tenant ON bm (tenant);
CREATE TABLE bm_plain (id int, tenant int, body text);
INSERT INTO bm_plain SELECT * FROM bm;
CREATE INDEX bm_plain_body ON bm_plain USING sift (body);
CREATE INDEX bm_plain_tenant ON bm_plain (tenant);
ANALYZE bm, bm_plain;

-- the plan of a bitmap heap scan: the filters and the recheck conditions it has
CREATE FUNCTION bm_plan(q text) RETURNS TABLE (node text) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '(Bitmap Heap Scan|Filter:|Recheck Cond:|BitmapOr)' THEN
			node := regexp_replace(trim(l), '''\d+''::oid', '''index''::oid');
			RETURN NEXT;
		END IF;
	END LOOP;
END $$;

-- the match is no filter of the rows: it is checked again where the bitmap is lossy
SELECT * FROM bm_plan('SELECT count(*) FROM bm WHERE body @@> ''run'' AND tenant = 6');
SELECT * FROM bm_plan('SELECT count(*) FROM bm WHERE body @@> ''run'' AND tenant < 20 AND id > 3000');
SELECT * FROM bm_plan('SELECT count(*) FROM bm_plain WHERE body @@> ''run'' AND tenant = 6');
-- an OR of the two is a bitmap OR, whose match is no exact condition of every row
SELECT * FROM bm_plan('SELECT count(*) FROM bm WHERE body @@> ''run'' OR tenant = 6');

-- the rows, and the scores, with the bitmap plan, a lossy bitmap (work_mem at the least) and the plan that evaluates every document
CREATE FUNCTION bm_run(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '' '' || coalesce(sum(id), 0) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION bm_bitmap(q text) RETURNS text LANGUAGE sql RETURN bm_run(q);
CREATE FUNCTION bm_lossy(q text) RETURNS text LANGUAGE sql SET work_mem = '64kB' RETURN bm_run(q);
CREATE FUNCTION bm_eval(q text) RETURNS text LANGUAGE sql SET enable_bitmapscan = off SET enable_seqscan = on RETURN bm_run(q);

SELECT n, bm_bitmap(q) AS bitmap, bm_bitmap(q) = bm_lossy(q) AS lossy_same, bm_bitmap(q) = bm_eval(q) AS eval_same
FROM (VALUES
	(1, 'SELECT id FROM bm WHERE body @@> ''run'' AND tenant = 6'),
	(2, 'SELECT id FROM bm WHERE body @@> ''run'' AND tenant < 30'),
	(3, 'SELECT id FROM bm WHERE body @@> ''running dog'' AND tenant < 30'),
	(4, 'SELECT id FROM bm WHERE body @@> ''"runs and"'' AND tenant < 30'),
	(5, 'SELECT id FROM bm WHERE body @@> ''jump NOT dog'' AND tenant < 30'),
	(6, 'SELECT id FROM bm WHERE body @@> ''run'' AND body @@> ''cat'' AND tenant < 30'),
	(7, 'SELECT id FROM bm WHERE body @@> ''run'' OR tenant = 6'),
	(8, 'SELECT id FROM bm WHERE body @@> ''zzz'' AND tenant < 30'),
	(9, 'SELECT id FROM bm_plain WHERE body @@> ''run'' AND tenant < 30'),
	(10, 'SELECT id FROM bm_plain WHERE body @@> ''runs'' AND tenant < 30')
) t(n, q) ORDER BY n;

-- a bitmap heap scan in a correlated sub-select, with the parameter in the match
SELECT t, (SELECT count(*) FROM bm WHERE body @@> t.w AND tenant < 30) AS n
FROM (VALUES (1, 'run'), (2, 'jump'), (3, 'cat'), (4, 'nothing')) t(t, w) ORDER BY t;

DROP FUNCTION bm_plan(text), bm_bitmap(text), bm_lossy(text), bm_eval(text), bm_run(text);
DROP TABLE bm, bm_plain;
DROP EXTENSION pg_sift;
