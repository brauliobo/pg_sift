-- W12: an OR of matches of one index is one scan of the query "a OR b", so sift.score() of a row is the score of that query.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

CREATE TABLE os (id int PRIMARY KEY, body text, grp int);
INSERT INTO os
SELECT i, concat_ws(' ', (ARRAY['alpha', 'beta', 'gamma', 'delta', 'omega', 'sigma', 'kappa', 'zeta'])[1 + i % 8],
                         (ARRAY['red', 'green', 'blue', 'cyan'])[1 + i % 4],
                         (ARRAY['wire', 'cable', 'rope'])[1 + i % 3],
                         repeat('pad ', i % 5)), i % 10
FROM generate_series(1, 2000) i;
CREATE INDEX os_idx ON os USING sift (body);
VACUUM ANALYZE os;

-- the plan is one ranked scan with both queries
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM os WHERE body @@> 'alpha' OR body @@> 'beta' ORDER BY sift.score(ctid) DESC, id LIMIT 5;
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM os WHERE body @@> 'alpha' OR body @@> 'beta' OR body @@> 'gamma' ORDER BY sift.score(ctid) DESC, id LIMIT 5;

-- the rows and the scores are those of the query written with OR
CREATE FUNCTION same_scores(or_sql text, q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(id || '':'' || round(s::numeric, 4), '' '' ORDER BY s DESC, id) FROM (SELECT id, sift.score(ctid) s FROM os WHERE %s ORDER BY sift.score(ctid) DESC, id LIMIT 25) t', or_sql) INTO a;
	EXECUTE format('SELECT string_agg(id || '':'' || round(s::numeric, 4), '' '' ORDER BY s DESC, id) FROM (SELECT id, sift.score(ctid) s FROM os WHERE body @@> %L ORDER BY sift.score(ctid) DESC, id LIMIT 25) t', q) INTO b;
	RETURN a = b AND a IS NOT NULL;
END $$;
SELECT same_scores($$body @@> 'alpha' OR body @@> 'beta'$$, 'alpha OR beta');
SELECT same_scores($$body @@> 'alpha red' OR body @@> 'zeta wire'$$, 'alpha red OR zeta wire');
SELECT same_scores($$body @@> '"red wire"' OR body @@> 'omega' OR body @@> 'kappa*'$$, '"red wire" OR omega OR kappa*');
SELECT same_scores($$body @@> 'alpha' OR body @@> 'alpha'$$, 'alpha');

-- a row that matches both branches scores once, as the OR of the query does (not the sum of two scans)
SELECT id, round(sift.score(ctid)::numeric, 4) AS both FROM os WHERE body @@> 'alpha' OR body @@> 'red' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT id, round(sift.score(ctid)::numeric, 4) AS one FROM os WHERE body @@> 'alpha OR red' ORDER BY sift.score(ctid) DESC, id LIMIT 3;

-- other quals ANDed with the OR, a top-k with an offset, the score as a filter
SELECT id FROM os WHERE (body @@> 'alpha' OR body @@> 'beta') AND grp = 3 ORDER BY sift.score(ctid) DESC, id LIMIT 4;
SELECT id FROM os WHERE (body @@> 'alpha' OR body @@> 'beta') ORDER BY sift.score(ctid) DESC, id LIMIT 3 OFFSET 2;
SELECT count(*) FROM os WHERE (body @@> 'alpha' OR body @@> 'beta') AND sift.score(ctid) > 0;
SELECT count(*) FROM os WHERE body @@> 'alpha' OR body @@> 'beta';
SELECT count(*) FROM os WHERE body @@> 'alpha OR beta';

-- the limit: 16 branches are one scan, more are left to PostgreSQL
CREATE FUNCTION uses_scan(n int) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; q text; found boolean := false;
BEGIN
	SELECT string_agg(format('body @@> %L', w), ' OR ') INTO q
	FROM unnest((ARRAY['alpha', 'beta', 'gamma', 'delta', 'omega', 'sigma', 'kappa', 'zeta', 'red', 'green', 'blue', 'cyan', 'wire', 'cable', 'rope', 'pad', 'x17'])[1:n]) w;
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT id FROM os WHERE ' || q || ' ORDER BY sift.score(ctid) DESC LIMIT 3'
	LOOP
		found := found OR line LIKE '%SiftScan%';
	END LOOP;
	RETURN found;
END $$;
SELECT n, uses_scan(n) FROM generate_series(15, 17) n;

-- a parameter in a branch, a branch that is not a match of this index, a NULL query
PREPARE p(text, text) AS SELECT id FROM os WHERE body @@> $1 OR body @@> $2 ORDER BY sift.score(ctid) DESC, id LIMIT 3;
EXECUTE p('alpha', 'beta');
EXECUTE p('alpha', NULL);
SELECT id FROM os WHERE (body @@> 'alpha' OR grp = 9) ORDER BY id LIMIT 3;
SELECT id FROM os WHERE body @@> 'alpha' OR body @@> NULL ORDER BY sift.score(ctid) DESC, id LIMIT 3;

-- writes in the buffer, a delete, a vacuum
INSERT INTO os VALUES (3001, 'alpha beta alpha beta', 1), (3002, 'beta', 2);
DELETE FROM os WHERE id IN (1, 2);
SELECT id, round(sift.score(ctid)::numeric, 4) FROM os WHERE body @@> 'alpha' OR body @@> 'beta' ORDER BY sift.score(ctid) DESC, id LIMIT 4;
SELECT same_scores($$body @@> 'alpha' OR body @@> 'beta'$$, 'alpha OR beta');
VACUUM os;
SELECT same_scores($$body @@> 'alpha' OR body @@> 'beta'$$, 'alpha OR beta');
SELECT sift.verify('os_idx', true);
DROP TABLE os;
DROP FUNCTION same_scores(text, text);
DROP FUNCTION uses_scan(int);
DROP EXTENSION pg_sift;
