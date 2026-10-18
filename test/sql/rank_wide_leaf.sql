-- A wildcard, regex or fuzzy leaf with more terms than sift.max_expansions reads as one term (the documents
-- that have a word it matches, tf the number of those words) like a wide prefix, instead of failing: the
-- top k equals the exact reference's, scores and ties included, with the limit at 1 (every multi-term leaf is
-- wide), at 4 (some are) and at its default (none is).
CREATE EXTENSION pg_sift;
\set sift_ref_lib 1
\ir test/sql/reference.sql

CREATE TABLE rw (id int PRIMARY KEY, body text);
INSERT INTO rw
SELECT i, 'tok' || lpad((i % 90)::text, 3, '0') || ' ing' || (i % 11) || ' walking' || (i % 5) || ' fil' || (i % 6) || 'tion' ||
	' venue' || (i % 4) || repeat(' pad', i % 6) || CASE WHEN i % 7 = 0 THEN ' tok005 walking' ELSE '' END
FROM generate_series(1, 400) i;
CREATE INDEX rw_idx ON rw USING sift (body);
ANALYZE rw;
SELECT sift_ref.load_index('rw_idx') > 0 AS loaded;

CREATE TEMP TABLE rw_q (q text);
INSERT INTO rw_q VALUES ('*ing*'), ('*tion'), ('/.*ing[0-9]+/'), ('/walk(ing|ed)[0-9]/'), ('venue~1'), ('tok05~2'), ('t?k00?'),
	('*tion AND NOT hotel1'), ('*ing* OR hotel3^2'), ('/.*k0[0-4].*/ walking'), ('tok1* OR *tion'), ('"tok005 walking"'), ('walking^3 *ing*');

CREATE FUNCTION rw_diff(q text, k int) RETURNS bigint LANGUAGE sql AS $$
	WITH got AS (SELECT row_number() OVER (ORDER BY score DESC, tid) AS rank, tid, score FROM
			(SELECT ctid AS tid, sift.score(ctid) AS score FROM rw WHERE body @@> q ORDER BY sift.score(ctid) DESC LIMIT k) g),
		want AS (SELECT rank, ctid AS tid, score FROM sift_ref.topk(q, k))
	SELECT count(*) FROM got FULL JOIN want USING (rank) WHERE got.tid IS DISTINCT FROM want.tid OR got.score IS DISTINCT FROM want.score
$$;
CREATE FUNCTION rw_count(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM rw WHERE body @@> $1' INTO n USING q;
	RETURN n;
END $$;

SELECT count(*) AS queries, min(rw_count(q)) > 0 AS all_match FROM rw_q;
SELECT q FROM rw_q WHERE rw_count(q) <> sift_ref.count(q);
SET sift.max_expansions = 4;
SELECT q FROM rw_q WHERE rw_count(q) <> sift_ref.count(q);
SELECT k, sum(rw_diff(q, k)) AS mismatches FROM unnest(ARRAY[1, 5, 25, 400]) k, rw_q GROUP BY k ORDER BY k;
SET sift.max_expansions = 1;
SELECT q FROM rw_q WHERE rw_count(q) <> sift_ref.count(q);
SELECT k, sum(rw_diff(q, k)) AS mismatches FROM unnest(ARRAY[1, 5, 25, 400]) k, rw_q GROUP BY k ORDER BY k;
SET sift.prefix_cache_size = 0;
SELECT k, sum(rw_diff(q, k)) AS mismatches FROM unnest(ARRAY[5, 400]) k, rw_q GROUP BY k ORDER BY k;
RESET sift.prefix_cache_size;
RESET sift.max_expansions;
SELECT k, sum(rw_diff(q, k)) AS mismatches FROM unnest(ARRAY[1, 5, 25, 400]) k, rw_q GROUP BY k ORDER BY k;

DROP FUNCTION rw_diff(text, int), rw_count(text);
DROP TABLE rw;
DROP SCHEMA sift_ref CASCADE;
DROP EXTENSION pg_sift;
