-- While less than 5% of a segment's docs were cleared since the last count of its dead postings, VACUUM leaves the table as it is and the dfs scale it to the live docs:
-- the scores stay within 1% of those of the same rows indexed afresh and the top 20 are the same docs. From 5% on, after a merge and after REINDEX the scores are exact.
CREATE EXTENSION pg_sift;
SELECT sift.version() IS NOT NULL AS library_loaded;

CREATE TABLE ds (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
-- a skewed vocabulary: u0 in most docs, u1 in half of them, up to a term of a single doc
CREATE FUNCTION ds_doc(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT 'u' || floor(12 * power((i * 7919 % 1000) / 1000.0, 3))::int || ' v' || floor(60 * power((i * 104729 % 1000) / 1000.0, 3))::int
		|| ' w' || i % 211 || ' z' || i % 5003 || repeat(' pad', i % 6) || CASE WHEN i % 9 = 0 THEN ' u0 v0' ELSE '' END
$$;
INSERT INTO ds SELECT i, ds_doc(i) FROM generate_series(1, 20000) i;
CREATE INDEX ds_idx ON ds USING sift (body);

CREATE TABLE ds_q (q text);
INSERT INTO ds_q VALUES ('u0'), ('u3'), ('v5'), ('w9'), ('u1 OR v2 OR w3'), ('u1 v1'), ('u0 v0'), ('"u0 v0"'), ('"v1 w1"'), ('z17 OR u0');
-- per query: the top 20 of ds and of a copy of its live rows indexed afresh, their largest score ratio, and whether every score is the same
CREATE FUNCTION ds_compare() RETURNS TABLE (q text, same_top20 bool, max_diff_below_1pct bool, same_scores bool) LANGUAGE plpgsql AS $$
DECLARE
	c record;
BEGIN
	DROP TABLE IF EXISTS ds_ref;
	CREATE TABLE ds_ref AS SELECT * FROM ds;
	CREATE INDEX ds_ref_idx ON ds_ref USING sift (body);
	FOR c IN SELECT * FROM ds_q LOOP
		q := c.q;
		SELECT bool_and(a.id IS NOT DISTINCT FROM b.id), coalesce(max(abs(a.s - b.s) / b.s), 0) < 0.01, bool_and(a.s IS NOT DISTINCT FROM b.s)
			INTO same_top20, max_diff_below_1pct, same_scores
			FROM (SELECT row_number() OVER (ORDER BY s DESC, id) n, id, s FROM (SELECT id, sift.score(ctid) s FROM ds WHERE body @@> c.q ORDER BY 2 DESC, 1 LIMIT 20) t) a
			FULL JOIN (SELECT row_number() OVER (ORDER BY s DESC, id) n, id, s FROM (SELECT id, sift.score(ctid) s FROM ds_ref WHERE body @@> c.q ORDER BY 2 DESC, 1 LIMIT 20) t) b USING (n);
		RETURN NEXT;
	END LOOP;
END $$;

-- 2% deleted, then VACUUM: the table of dead postings is not counted again
DELETE FROM ds WHERE id % 50 = 7;
VACUUM ds;
SELECT dead_docs FROM sift.segments('ds_idx') WHERE kind = 'immutable';
SELECT * FROM ds_compare();
SELECT sift.verify('ds_idx', true);
SELECT bool_or(NOT same_scores) AS scores_differ FROM ds_compare();
SELECT bool_and((SELECT count(*) FROM ds WHERE body @@> q) = (SELECT count(*) FROM ds_ref WHERE body @@> q)) AS counts_exact FROM ds_q;
-- 4% more: 6% since the last count, so VACUUM counts again and the scores are exact
DELETE FROM ds WHERE id % 25 = 3;
VACUUM ds;
SELECT sift.verify('ds_idx', true);
SELECT bool_and(same_scores) AS exact FROM ds_compare();
-- 1% more is left uncounted; a merge drops the dead docs and the scores are exact again
DELETE FROM ds WHERE id % 100 = 11;
VACUUM ds;
SELECT bool_or(NOT same_scores) AS scores_differ, bool_and(same_top20 AND max_diff_below_1pct) AS close FROM ds_compare();
SELECT bool_and((SELECT count(*) FROM ds WHERE body @@> q) = (SELECT count(*) FROM ds_ref WHERE body @@> q)) AS counts_exact FROM ds_q;
SELECT sift.merge('ds_idx', true) > 0 AS merged;
SELECT dirty, dead_docs FROM sift.segments('ds_idx') WHERE kind = 'immutable';
SELECT bool_and(same_scores) AS exact FROM ds_compare();
-- REINDEX
DELETE FROM ds WHERE id % 100 = 12;
VACUUM ds;
REINDEX INDEX ds_idx;
SELECT bool_and(same_scores) AS exact FROM ds_compare();

DROP FUNCTION ds_compare(), ds_doc(int);
DROP TABLE ds, ds_ref, ds_q;
DROP EXTENSION pg_sift;
