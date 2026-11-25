-- The ranked scans of the partitions under a Merge Append under a LIMIT of k rows share one floor (sift.parallel_scan): the k-th best score of the
-- partition searched first, which the searches after it start above, so their blocks that cannot beat it are never scored. The k best rows are those of the
-- serial plan (every partition searching alone), for every k, with write buffers, dead rows, a rescan, and the plans that must not share (ties by key, no limit, one partition).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.parallel_scan = on;

CREATE TABLE pp (id int, g int, body text) PARTITION BY RANGE (id);
CREATE TABLE pp0 PARTITION OF pp FOR VALUES FROM (0) TO (10000) WITH (autovacuum_enabled = off);
CREATE TABLE pp1 PARTITION OF pp FOR VALUES FROM (10000) TO (20000) WITH (autovacuum_enabled = off);
CREATE TABLE pp2 PARTITION OF pp FOR VALUES FROM (20000) TO (30000) WITH (autovacuum_enabled = off);
INSERT INTO pp SELECT i, i % 7, concat_ws(' ', repeat('fox ', CASE WHEN i < 10000 THEN 4 + i % 5 ELSE 1 + i % 5 END), CASE WHEN i % 11 = 0 THEN 'quick' END,
	CASE WHEN i % 5 = 0 THEN 'brown' END, 'w' || i % 101, CASE WHEN i < 10000 THEN NULL ELSE repeat('filler words ', 3 + i % 17) END)
FROM generate_series(1, 29999) i;
SET client_min_messages = warning;
CREATE INDEX pp_body ON pp USING sift (body);
RESET client_min_messages;
ANALYZE pp;

CREATE FUNCTION pp_scores(q text, k int, par bool) RETURNS float4[] LANGUAGE plpgsql AS $$
DECLARE r float4[];
BEGIN
	EXECUTE format('SET sift.parallel_scan = %s', CASE WHEN par THEN 'on' ELSE 'off' END);
	EXECUTE format('SELECT array_agg(round(s::numeric, 4)::float4 ORDER BY s DESC) FROM (SELECT sift.score(ctid) AS s FROM pp WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT %s) x', q, k) INTO r;
	SET sift.parallel_scan = on;
	RETURN r;
END $$;

CREATE FUNCTION pp_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Merge Append|Shared Floor|Includes Ties' THEN r := r || btrim(l) || ';'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT pp_plan('SELECT id FROM pp WHERE body @@> ''fox quick'' ORDER BY sift.score(ctid) DESC LIMIT 10');

CREATE FUNCTION pp_candidates(par bool) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	EXECUTE format('SET sift.parallel_scan = %s', CASE WHEN par THEN 'on' ELSE 'off' END);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT id FROM pp WHERE body @@> ''fox quick'' ORDER BY sift.score(ctid) DESC LIMIT 10' LOOP
		n := n + coalesce((regexp_match(l, ', (\d+) candidates'))[1]::int, 0);
	END LOOP;
	SET sift.parallel_scan = on;
	RETURN n;
END $$;
SELECT pp_candidates(true) < pp_candidates(false) AS floor_prunes_candidates;

CREATE TABLE pp_q (n int, q text, k int);
INSERT INTO pp_q VALUES (1, 'fox quick', 1), (2, 'fox quick', 10), (3, 'fox quick', 200), (4, 'fox brown', 25), (5, 'fox', 50), (6, 'quick OR brown', 40), (7, 'fox AND NOT quick', 15),
	(8, 'w5', 30), (9, 'fox w7 OR quick w7', 20), (10, 'nomatchword', 10), (11, 'fox quick', 20000);
SELECT count(*) > 0 AS reset FROM sift._test_read_stats(true);
SELECT n, pp_scores(q, k, true) IS NOT DISTINCT FROM pp_scores(q, k, false) AS same_scores, cardinality(pp_scores(q, k, true)) AS rows FROM pp_q ORDER BY n;
SELECT value >= 2 * 10 AS searches_started_above_a_floor FROM sift._test_read_stats() WHERE key = 'floor_searches';

\echo -- the rows are the rows of the scores: each returned row scores what the serial plan says
SELECT count(*) AS rows, count(*) FILTER (WHERE round(s::numeric, 4)::float4 = ANY (pp_scores('fox quick', 60, false))) AS in_serial_top
FROM (SELECT sift.score(ctid) s FROM pp WHERE body @@> 'fox quick' ORDER BY sift.score(ctid) DESC LIMIT 60) x;

\echo -- a floor is not kept between executions of a plan, nor across a rescan
CREATE FUNCTION pp_top(q text) RETURNS float4[] LANGUAGE plpgsql AS $$
DECLARE r float4[];
BEGIN
	SELECT array_agg(round(s::numeric, 4)::float4 ORDER BY s DESC) INTO r FROM (SELECT sift.score(ctid) AS s FROM pp WHERE body @@> q ORDER BY sift.score(ctid) DESC LIMIT 12) x;
	RETURN r;
END $$;
SELECT pp_top('fox quick') = pp_scores('fox quick', 12, false) AS first, pp_top('fox quick') = pp_scores('fox quick', 12, false) AS second,
	pp_top('quick OR brown') = pp_scores('quick OR brown', 12, false) AS other_query, pp_top('w5') = pp_scores('w5', 12, false) AS another;
SELECT count(DISTINCT s) AS one_answer_per_outer_row FROM (SELECT g, array_agg(round(sc::numeric, 4)::float4 ORDER BY sc DESC)::text AS s
	FROM generate_series(1, 4) g, LATERAL (SELECT sift.score(ctid) sc FROM pp WHERE body @@> 'fox quick' AND g > 0 ORDER BY sift.score(ctid) DESC LIMIT 7) l GROUP BY g) x;
SELECT (SELECT array_agg(round(sc::numeric, 4)::float4 ORDER BY sc DESC) FROM (SELECT sift.score(ctid) sc FROM pp WHERE body @@> 'fox quick' ORDER BY sift.score(ctid) DESC LIMIT 7) y) = pp_scores('fox quick', 7, false) AS rescan_is_serial;

\echo -- write buffers, dead rows and moved rows
INSERT INTO pp SELECT 40 + i, 0, 'fox fox fox fox fox fox fox fox quick' FROM generate_series(1, 6) i;
INSERT INTO pp SELECT 10040 + i, 0, 'fox quick fox quick' FROM generate_series(1, 6) i;
DELETE FROM pp WHERE id % 13 = 0;
UPDATE pp SET body = body || ' brown' WHERE id % 29 = 0;
SELECT n, pp_scores(q, k, true) IS NOT DISTINCT FROM pp_scores(q, k, false) AS same_scores FROM pp_q ORDER BY n;
BEGIN;
DELETE FROM pp WHERE id BETWEEN 1 AND 20000 AND id % 2 = 0;
INSERT INTO pp SELECT 20000 + i, 1, 'fox fox quick brown' FROM generate_series(1, 9) i;
SELECT n, pp_scores(q, k, true) IS NOT DISTINCT FROM pp_scores(q, k, false) AS same_scores FROM pp_q WHERE n IN (2, 3, 5, 11) ORDER BY n;
ROLLBACK;

\echo -- no floor: ties by key under an incremental sort, no limit, one partition, and the setting off
SELECT pp_plan('SELECT id FROM pp WHERE body @@> ''fox quick'' ORDER BY sift.score(ctid) DESC, id LIMIT 10');
SELECT pp_plan('SELECT id FROM pp WHERE body @@> ''fox quick'' ORDER BY sift.score(ctid) DESC');
SELECT pp_plan('SELECT id FROM pp WHERE body @@> ''fox quick'' AND id < 10000 ORDER BY sift.score(ctid) DESC LIMIT 10');
SET sift.parallel_scan = off;
SELECT pp_plan('SELECT id FROM pp WHERE body @@> ''fox quick'' ORDER BY sift.score(ctid) DESC LIMIT 10');
SET sift.parallel_scan = on;
DROP TABLE pp, pp_q;
DROP FUNCTION pp_scores(text, int, bool), pp_plan(text), pp_candidates(bool), pp_top(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
