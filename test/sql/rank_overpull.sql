-- A consumer that drops most rows pulls past the scan's k hint: the search repeats with more hits (16 times as
-- many below 4096, 4 times beyond), and the rows and their order stay those of the full ranking (DESIGN.md (f))
CREATE EXTENSION pg_sift;

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

CREATE TABLE ov (id int, body text);
INSERT INTO ov SELECT i, repeat('alpha ', 1 + i % 5) || repeat('beta ', i % 4) || 'w' || i % 311 FROM generate_series(1, 6000) i;
CREATE INDEX ov_body ON ov USING sift (body);
CREATE TABLE ov_keep (id int PRIMARY KEY);
INSERT INTO ov_keep SELECT i FROM generate_series(1, 6000) i WHERE i % 67 = 0;
VACUUM ANALYZE ov;
ANALYZE ov_keep;

-- the join keeps 1.5% of the rows: 30 of them need about 2000 pulled, the hint is 30, so the search grows twice
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT sift_plan('SELECT o.id FROM ov o JOIN ov_keep k ON k.id = o.id WHERE o.body @@> ''alpha OR beta''
	ORDER BY sift.score(o.ctid) DESC LIMIT 30') ~ 'Nested Loop(.|\n)*SiftScan' AS nested_loop_over_scan;

-- the scores of the 30 best joined rows are those of the full ranking filtered by the join
SELECT (SELECT array_agg(s) FROM (SELECT sift.score(o.ctid) AS s FROM ov o JOIN ov_keep k ON k.id = o.id
		WHERE o.body @@> 'alpha OR beta' ORDER BY sift.score(o.ctid) DESC LIMIT 30) j)
	= (SELECT array_agg(s) FROM (SELECT t.s FROM (SELECT id, sift.score(ctid) AS s FROM ov WHERE body @@> 'alpha OR beta'
		ORDER BY sift.score(ctid) DESC OFFSET 0) t JOIN ov_keep k ON k.id = t.id ORDER BY t.s DESC LIMIT 30) r) AS top30;

-- drained: every joined row comes out, best first, as many as the full ranking holds
SELECT (SELECT array_agg(s) FROM (SELECT sift.score(o.ctid) AS s FROM ov o JOIN ov_keep k ON k.id = o.id
		WHERE o.body @@> 'alpha OR beta' ORDER BY sift.score(o.ctid) DESC) j)
	= (SELECT array_agg(s) FROM (SELECT t.s FROM (SELECT id, sift.score(ctid) AS s FROM ov WHERE body @@> 'alpha OR beta'
		ORDER BY sift.score(ctid) DESC OFFSET 0) t JOIN ov_keep k ON k.id = t.id ORDER BY t.s DESC) r) AS drained,
	(SELECT sum(o.id) FROM ov o JOIN ov_keep k ON k.id = o.id WHERE o.body @@> 'alpha OR beta') AS rows_sum;

RESET enable_hashjoin;
RESET enable_mergejoin;
DROP FUNCTION sift_plan(text);
DROP TABLE ov, ov_keep;
DROP EXTENSION pg_sift;
