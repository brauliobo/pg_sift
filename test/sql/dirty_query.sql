-- Queries over a segment with vacuumed documents (dirty superblocks): the liveness bits of a superblock are read once
-- and spread over the offsets of each page, so pages of 221 documents (four words of offsets), superblocks that start
-- in the middle of a byte of the bits (256 pages of 221 or 222 rows) and pages that are all dead must give what the
-- live rows give. Every query is compared with a predicate on the rows: the count, the unranked rows, and the ranked
-- rows (where the matches of a length score alike, so the top rows are the first ids of the shorter rows, then of the
-- rows with pad).
CREATE EXTENSION pg_sift;
SET enable_seqscan = off;

-- two one-letter terms per row (b..h by id % 7, j..v by id % 13), and pad in one row of ten
CREATE FUNCTION dq_text(id int) RETURNS text LANGUAGE sql IMMUTABLE
	RETURN chr(98 + id % 7) || ' ' || chr(106 + id % 13) || CASE WHEN id % 10 = 0 THEN ' pad' ELSE '' END;
CREATE TABLE dq (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO dq SELECT i, dq_text(i) FROM generate_series(1, 70000) i;
CREATE INDEX dq_idx ON dq USING sift (body);
-- a random third of the rows, and all of the pages around the end of the first superblock
DELETE FROM dq WHERE hashint4(id) % 3 = 0 OR id BETWEEN 54000 AND 58000;
VACUUM dq;
SELECT dirty, sb_hi > sb_lo AS superblocks, dead_docs > 20000 AS dead FROM sift.segments('dq_idx');

CREATE TABLE dq_cases (q text, pred text, ties bool);
INSERT INTO dq_cases VALUES
	('d', 'id % 7 = 2', true),
	('m d', 'id % 13 = 3 AND id % 7 = 2', true),
	('c OR e', 'id % 7 IN (1, 3)', false),
	('d NOT m', 'id % 7 = 2 AND id % 13 <> 3', true),
	('pad', 'id % 10 = 0', true),
	('p*', 'id % 13 = 6 OR id % 10 = 0', false),
	('missing OR v', 'id % 13 = 12', true);

CREATE FUNCTION dq_check() RETURNS TABLE (q text, count_ok bool, rows_ok bool, top_ok bool, deep_ok bool) LANGUAGE plpgsql AS $$
DECLARE
	c record;
BEGIN
	FOR c IN SELECT * FROM dq_cases LOOP
		q := c.q;
		top_ok := NULL;
		deep_ok := NULL;
		EXECUTE format('SELECT (SELECT count(*) FROM dq WHERE body @@> %L) = (SELECT count(*) FROM dq WHERE %s)', c.q, c.pred) INTO count_ok;
		EXECUTE format('SELECT NOT EXISTS (SELECT 1 FROM (SELECT id FROM dq WHERE body @@> %L EXCEPT ALL SELECT id FROM dq WHERE %s) a)
			AND NOT EXISTS (SELECT 1 FROM (SELECT id FROM dq WHERE %s EXCEPT ALL SELECT id FROM dq WHERE body @@> %L) b)',
			c.q, c.pred, c.pred, c.q) INTO rows_ok;
		IF c.ties THEN
			EXECUTE format('SELECT (SELECT array_agg(id) FROM (SELECT id FROM dq WHERE body @@> %L ORDER BY sift.score(ctid) DESC, id LIMIT 7) t) IS NOT DISTINCT FROM
				(SELECT array_agg(id) FROM (SELECT id FROM dq WHERE %s ORDER BY id %% 10 = 0, id LIMIT 7) t)', c.q, c.pred) INTO top_ok;
			EXECUTE format('SELECT (SELECT array_agg(id) FROM (SELECT id FROM dq WHERE body @@> %L ORDER BY sift.score(ctid) DESC, id LIMIT 5 OFFSET 2000) t) IS NOT DISTINCT FROM
				(SELECT array_agg(id) FROM (SELECT id FROM dq WHERE %s ORDER BY id %% 10 = 0, id LIMIT 5 OFFSET 2000) t)', c.q, c.pred) INTO deep_ok;
		END IF;
		RETURN NEXT;
	END LOOP;
END $$;

SELECT * FROM dq_check();
SELECT sift.verify('dq_idx', true);

-- more deletions into the vacuumed superblocks, then a promoted segment of new rows beside them
DELETE FROM dq WHERE id % 11 = 4;
VACUUM dq;
INSERT INTO dq SELECT i, dq_text(i) FROM generate_series(70001, 72000) i;
SELECT sift.promote('dq_idx') > 0 AS promoted;
SELECT * FROM dq_check();
SELECT sift.verify('dq_idx', true);

-- the same rows once the segments are rewritten
SELECT sift.merge('dq_idx', true) > 0 AS merged;
SELECT count(*) FILTER (WHERE dirty) AS dirty FROM sift.segments('dq_idx') WHERE kind = 'immutable';
SELECT * FROM dq_check();

-- Counts over a dirty segment whose superblocks differ: the first is dirty, the others are clean (a tenth of each page is
-- filled, so four superblocks of 5632 rows are there to hand to parallel workers). A term on every page is counted for
-- the whole group, a term on a few pages page by page, and the pages of rows deleted since the VACUUM are left to the heap.
CREATE TABLE dq2 (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off, fillfactor = 10);
INSERT INTO dq2 SELECT i, chr(98 + i % 7) || ' ' || chr(106 + i % 13) || CASE WHEN i % 997 = 0 THEN ' z' ELSE '' END FROM generate_series(1, 18000) i;
CREATE INDEX dq2_idx ON dq2 USING sift (body);
DELETE FROM dq2 WHERE id <= 3000 AND id % 3 <> 0;
VACUUM dq2;
SELECT dirty, sb_hi - sb_lo + 1 > 2 AS superblocks FROM sift.segments('dq2_idx');
-- the first superblock is the only dirty one: the SB bitmap of the liveness decides, without reading the others
SELECT sift.verify('dq2_idx', true);

CREATE TABLE dq2_cases (q text, pred text);
INSERT INTO dq2_cases VALUES
	('d', 'id % 7 = 2'),
	('m d', 'id % 13 = 3 AND id % 7 = 2'),
	('c OR e', 'id % 7 IN (1, 3)'),
	('z', 'id % 997 = 0'),
	('z OR v', 'id % 997 = 0 OR id % 13 = 12'),
	('z c', 'id % 997 = 0 AND id % 7 = 1');
CREATE FUNCTION dq2_check() RETURNS TABLE (q text, count_ok bool, par_ok bool) LANGUAGE plpgsql AS $$
DECLARE
	c record;
BEGIN
	FOR c IN SELECT * FROM dq2_cases LOOP
		q := c.q;
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE format('SELECT (SELECT count(*) FROM dq2 WHERE body @@> %L) = (SELECT count(*) FROM dq2 WHERE %s)', c.q, c.pred) INTO count_ok;
		PERFORM set_config('max_parallel_workers_per_gather', '2', true);
		EXECUTE format('SELECT (SELECT count(*) FROM dq2 WHERE body @@> %L) = (SELECT count(*) FROM dq2 WHERE %s)', c.q, c.pred) INTO par_ok;
		RETURN NEXT;
	END LOOP;
END $$;
SET sift.parallel_scan = on;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
ALTER TABLE dq2 SET (parallel_workers = 2);
SELECT * FROM dq2_check();
-- the parallel counts above are parallel
DO $$
DECLARE
	l text;
	p bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT count(*) FROM dq2 WHERE body @@> ''d''' LOOP
		p := p OR l LIKE '%Gather%';
	END LOOP;
	RAISE NOTICE 'parallel plan: %', p;
END $$;

-- rows deleted since the VACUUM sit on pages that are not all-visible
DELETE FROM dq2 WHERE id BETWEEN 3001 AND 3300 OR id BETWEEN 9001 AND 9300;
SELECT * FROM dq2_check();
-- the scalar kernels count the same
SET sift.simd = scalar;
SELECT * FROM dq2_check();
RESET sift.simd;

DROP TABLE dq, dq_cases, dq2, dq2_cases;
DROP FUNCTION dq_check(), dq_text(int), dq2_check();
DROP EXTENSION pg_sift;
