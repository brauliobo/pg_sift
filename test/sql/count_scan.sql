-- count(*) over the index (SiftCount): visibility map pages from bitmaps, other pages through
-- the heap, equal to a sequential count under dead, deleted and aborted rows
CREATE EXTENSION pg_sift;

CREATE TABLE c (id int PRIMARY KEY, body text, grp int);
INSERT INTO c SELECT i, (ARRAY['quick brown fox', 'lazy dog', 'the quick fox', 'brown dog', 'fox'])[1 + i % 5]
	|| ' w' || i % 7, i % 3 FROM generate_series(1, 2000) i;
INSERT INTO c VALUES (2001, NULL, 0), (2002, '', 0);
CREATE INDEX c_idx ON c USING sift (body);
VACUUM ANALYZE c;

-- the count through the index and by evaluating every document
CREATE FUNCTION via_scan(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM c WHERE body @@> $1' INTO n USING q;
	RETURN n;
END $$;
CREATE FUNCTION via_seq(q text) RETURNS bigint LANGUAGE sql
	SET sift.enable_count = off SET sift.enable_scan = off SET enable_bitmapscan = off SET enable_indexscan = off
	RETURN (SELECT count(*) FROM c WHERE body @@> q);
CREATE FUNCTION same(q text) RETURNS text LANGUAGE sql
	RETURN via_scan(q) || CASE WHEN via_scan(q) = via_seq(q) THEN ' ok' ELSE ' MISMATCH ' || via_seq(q) END;

CREATE FUNCTION sift_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN rtrim(r, E'\n');
END $$;

CREATE FUNCTION all_same() RETURNS TABLE (q text, result text) LANGUAGE sql AS $$
	SELECT q, same(q) FROM (VALUES ('fox'), ('quick fox'), ('fox NOT quick'), ('dog OR fox'), ('"quick fox"'),
		('"brown fox" OR dog'), ('fo*'), ('/d.g/'), ('lazzy~1'), ('w3'), ('nothing'), ('fox^2')) v(q)
$$;

SELECT * FROM all_same();
SELECT sift_plan('SELECT count(*) FROM c WHERE body @@> ''fox''');
SELECT count(*) FROM c WHERE body @@> NULL::text;

-- dead rows: deleted but not vacuumed (the deleted pages leave the visibility map, the others stay)
DELETE FROM c WHERE id % 10 = 0;
SELECT * FROM all_same();
DELETE FROM c WHERE id BETWEEN 1500 AND 1800;
SELECT * FROM all_same();

-- uncommitted and rolled back deletes
BEGIN;
DELETE FROM c WHERE body @@> 'fox';
SELECT count(*) FROM c WHERE body @@> 'fox';
SAVEPOINT s1;
DELETE FROM c;
SELECT count(*) FROM c WHERE body @@> 'dog';
ROLLBACK TO s1;
SELECT count(*) FROM c WHERE body @@> 'dog';
ROLLBACK;
SELECT * FROM all_same();

-- not every statement is a plain count: other aggregates, groups, filters keep the other plans
SELECT sift_plan('SELECT count(*), max(id) FROM c WHERE body @@> ''fox''');
SELECT sift_plan('SELECT grp, count(*) FROM c WHERE body @@> ''fox'' GROUP BY grp');
SELECT sift_plan('SELECT count(*) FROM c WHERE body @@> ''fox'' AND grp = 1');
SELECT count(*) FROM c WHERE body @@> 'fox' AND grp = 1;
SELECT count(*), count(id) FROM c WHERE body @@> 'fox';
SET sift.enable_count = off;
SELECT sift_plan('SELECT count(*) FROM c WHERE body @@> ''fox''') !~ 'SiftCount' AS other_plan;
RESET sift.enable_count;

-- vacuum clears the deleted documents' bits (dirty superblocks, pages back in the visibility map), and
-- inserted or updated rows put pages out of it again next to them
VACUUM c;
SELECT * FROM all_same();
INSERT INTO c SELECT 3000 + i, 'lazy fox w' || i, 1 FROM generate_series(1, 30) i;
UPDATE c SET grp = grp + 1 WHERE id % 97 = 0;
SELECT * FROM all_same();
VACUUM c;
SELECT * FROM all_same();

DROP TABLE c CASCADE;
DROP FUNCTION via_scan(text), sift_plan(text), all_same();
DROP EXTENSION pg_sift;
