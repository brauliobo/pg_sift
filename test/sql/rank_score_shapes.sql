-- Where sift.score() may sit: the scan computes it wherever PostgreSQL places the projection of its relation. A subquery or a WITH query
-- that PostgreSQL pulls up, a HAVING that reads the score alone and the source of an INSERT ... SELECT give the rows of the same query
-- ranked in a subquery that PostgreSQL leaves alone (OFFSET 0, the reference). The statements that change or lock the rows they read
-- re-check them, which a scan cannot: a subquery or WITH query of such a statement is planned apart, and a score read straight from the
-- relation the statement joins is an error that says what to write.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
\set VERBOSITY terse

CREATE FUNCTION ss_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION ss_rows(q text) RETURNS jsonb LANGUAGE sql AS $$ SELECT sift_helpers.rows_as_jsonb(q) $$;

-- the query and the reference return the same rows in the same order; scan: the query's plan holds the scan
CREATE FUNCTION ss_same(q text, ref text, OUT n int, OUT same boolean, OUT scan boolean) LANGUAGE plpgsql AS $$
BEGIN
	n := jsonb_array_length(ss_rows(q));
	same := ss_rows(q) = ss_rows(ref);
	scan := ss_plan(q) ~ 'SiftScan';
END $$;

CREATE TABLE ss_items (id int, grp int, body text);
INSERT INTO ss_items SELECT i, i % 5, repeat('alpha ', 1 + i % 5) || repeat('beta ', i % 4) || 'w' || i % 31 FROM generate_series(1, 2000) i;
CREATE INDEX ss_items_body ON ss_items USING sift (body);
CREATE TABLE ss_other (id int PRIMARY KEY, tag text);
INSERT INTO ss_other SELECT i, 't' || i FROM generate_series(1, 2000, 3) i;
CREATE TABLE ss_dst (id int, s float4);
VACUUM ANALYZE ss_items, ss_other;

-- the reference of every shape: the matches of 'alpha' with their scores, in a subquery PostgreSQL does not flatten
CREATE VIEW ss_ref AS SELECT id, grp, s FROM (SELECT id, grp, sift.score(ctid) AS s FROM ss_items WHERE body @@> 'alpha' OFFSET 0) q;

-- 1. shapes PostgreSQL flattens in a SELECT: the scan is in the plan and the rows are the reference's
SELECT * FROM ss_same('SELECT id, s FROM (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> ''alpha'') q ORDER BY s DESC, id LIMIT 7',
	'SELECT id, s FROM ss_ref ORDER BY s DESC, id LIMIT 7');
SELECT * FROM ss_same('WITH q AS (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> ''alpha'') SELECT * FROM q ORDER BY s DESC, id LIMIT 7',
	'SELECT id, s FROM ss_ref ORDER BY s DESC, id LIMIT 7');
SELECT * FROM ss_same('SELECT id, s FROM (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> ''alpha'') q WHERE s > 0.0002 ORDER BY id',
	'SELECT id, s FROM ss_ref WHERE s > 0.0002 ORDER BY id');
SELECT * FROM ss_same('SELECT q.id, q.s, o.tag FROM (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> ''alpha'') q JOIN ss_other o USING (id) ORDER BY q.s DESC, q.id LIMIT 5',
	'SELECT q.id, q.s, o.tag FROM ss_ref q JOIN ss_other o USING (id) ORDER BY q.s DESC, q.id LIMIT 5');
SELECT * FROM ss_same('WITH q AS (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> ''alpha'') SELECT o.id, q.s FROM ss_other o LEFT JOIN q USING (id) ORDER BY q.s DESC NULLS LAST, o.id LIMIT 5',
	'SELECT o.id, q.s FROM ss_other o LEFT JOIN ss_ref q USING (id) ORDER BY q.s DESC NULLS LAST, o.id LIMIT 5');
SELECT * FROM ss_same('SELECT id, row_number() OVER (ORDER BY sift.score(ctid) DESC, id) AS rn FROM ss_items WHERE body @@> ''alpha'' ORDER BY 2 LIMIT 5',
	'SELECT id, row_number() OVER (ORDER BY s DESC, id) AS rn FROM ss_ref ORDER BY 2 LIMIT 5');
SELECT * FROM ss_same('SELECT DISTINCT ON (grp) grp, id, sift.score(ctid) AS s FROM ss_items WHERE body @@> ''alpha'' ORDER BY grp, sift.score(ctid) DESC, id',
	'SELECT DISTINCT ON (grp) grp, id, s FROM ss_ref ORDER BY grp, s DESC, id');

-- 2. a score read in HAVING only: the scan outputs it for the aggregate that HAVING reads
SELECT * FROM ss_same('SELECT grp FROM ss_items WHERE body @@> ''alpha'' GROUP BY grp HAVING max(sift.score(ctid)) > 0.00028 ORDER BY 1',
	'SELECT grp FROM ss_ref GROUP BY grp HAVING max(s) > 0.00028 ORDER BY 1');
SELECT * FROM ss_same('SELECT grp, count(*) AS n FROM ss_items WHERE body @@> ''alpha'' GROUP BY grp HAVING avg(sift.score(ctid)) > 0.00025 ORDER BY 1',
	'SELECT grp, count(*) AS n FROM ss_ref GROUP BY grp HAVING avg(s) > 0.00025 ORDER BY 1');

-- 3. INSERT ... SELECT reads the rows it inserts: the scan serves it, with or without a limit
INSERT INTO ss_dst SELECT id, sift.score(ctid) FROM ss_items WHERE body @@> 'alpha';
SELECT count(*) = (SELECT count(*) FROM ss_ref) AS all_rows,
	   ss_rows('SELECT id, s FROM ss_dst ORDER BY id') = ss_rows('SELECT id, s::float4 FROM ss_ref ORDER BY id') AS same_scores FROM ss_dst;
SELECT ss_plan('INSERT INTO ss_dst SELECT id, sift.score(ctid) FROM ss_items WHERE body @@> ''alpha''') ~ 'SiftScan' AS insert_scan;
TRUNCATE ss_dst;
INSERT INTO ss_dst SELECT id, sift.score(ctid) FROM ss_items WHERE body @@> 'alpha' ORDER BY sift.score(ctid) DESC, id LIMIT 5;
SELECT ss_rows('SELECT id, s FROM ss_dst ORDER BY s DESC, id') = ss_rows('SELECT id, s::float4 FROM ss_ref ORDER BY s DESC, id LIMIT 5') AS limited_insert;
-- ON CONFLICT: the source is read as in any INSERT
CREATE UNIQUE INDEX ss_dst_id ON ss_dst (id);
INSERT INTO ss_dst SELECT id, 0 FROM ss_items WHERE body @@> 'alpha' AND sift.score(ctid) > 0 ORDER BY id LIMIT 8
	ON CONFLICT (id) DO UPDATE SET s = excluded.s + 1;
SELECT count(*) AS rows_after_conflict FROM ss_dst;
DROP INDEX ss_dst_id;

-- 4. UPDATE, DELETE, MERGE and FOR UPDATE re-check the rows they read: a subquery or WITH query that reads the score is planned
-- apart (flattened, it would be refused), and its rows are the reference's
TRUNCATE ss_dst;
INSERT INTO ss_dst SELECT id, -1 FROM ss_items WHERE id % 10 = 0;
UPDATE ss_dst d SET s = q.s FROM (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> 'alpha') q WHERE d.id = q.id;
SELECT ss_rows('SELECT id, s FROM ss_dst ORDER BY id') = ss_rows('SELECT d.id, r.s::float4 AS s FROM ss_dst d JOIN ss_ref r USING (id) ORDER BY id') AS update_from_subquery,
	   count(*) FILTER (WHERE s < 0) AS not_updated FROM ss_dst;
UPDATE ss_dst SET s = -1;
WITH q AS (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> 'alpha') UPDATE ss_dst d SET s = q.s FROM q WHERE d.id = q.id;
SELECT ss_rows('SELECT id, s FROM ss_dst ORDER BY id') = ss_rows('SELECT d.id, r.s::float4 AS s FROM ss_dst d JOIN ss_ref r USING (id) ORDER BY id') AS update_from_cte;
UPDATE ss_dst SET s = -1;
MERGE INTO ss_dst d USING (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> 'alpha') q ON d.id = q.id WHEN MATCHED THEN UPDATE SET s = q.s;
SELECT ss_rows('SELECT id, s FROM ss_dst ORDER BY id') = ss_rows('SELECT d.id, r.s::float4 AS s FROM ss_dst d JOIN ss_ref r USING (id) ORDER BY id') AS merge_using_subquery;
DELETE FROM ss_dst d USING (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> 'alpha') q WHERE d.id = q.id AND q.s > 0.00028;
SELECT count(*) = (SELECT count(*) FROM ss_items WHERE id % 10 = 0 AND id IN (SELECT id FROM ss_ref WHERE s <= 0.00028)) AS delete_using_subquery FROM ss_dst;
SELECT id FROM (SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> 'alpha') q JOIN ss_other o USING (id) ORDER BY s DESC, id LIMIT 2 FOR UPDATE OF o;
-- the same with a view
CREATE VIEW ss_scored AS SELECT id, sift.score(ctid) AS s FROM ss_items WHERE body @@> 'alpha';
UPDATE ss_dst d SET s = v.s FROM ss_scored v WHERE d.id = v.id;
SELECT ss_rows('SELECT id, s FROM ss_dst ORDER BY id') = ss_rows('SELECT d.id, r.s::float4 AS s FROM ss_dst d JOIN ss_ref r USING (id) ORDER BY id') AS update_from_view;

-- 5. what stays an error says how to write it
\set VERBOSITY default
UPDATE ss_dst SET s = sift.score(ctid) WHERE id = 10;
UPDATE ss_dst d SET s = sift.score(i.ctid) FROM ss_items i WHERE i.body @@> 'alpha' AND i.id = d.id;
DELETE FROM ss_dst d USING ss_items i WHERE i.body @@> 'alpha' AND i.id = d.id AND sift.score(i.ctid) > 1;
SELECT i.id FROM ss_items i JOIN ss_other o USING (id) WHERE i.body @@> 'alpha' ORDER BY sift.score(i.ctid) DESC LIMIT 2 FOR UPDATE OF o;
INSERT INTO ss_dst SELECT id, sift.score(ctid) FROM ss_items WHERE body @@> 'alpha' FOR UPDATE;
-- a score outside any scan keeps its error
SELECT sift.score(ctid) FROM ss_items;

DROP VIEW ss_scored, ss_ref;
DROP TABLE ss_items, ss_other, ss_dst;
DROP FUNCTION ss_same(text, text), ss_rows(text), ss_plan(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
