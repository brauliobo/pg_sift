-- The group table of a scan is in memory, and a hash aggregate gets work_mem x hash_mem_multiplier. When the groups the planner expects
-- (with the bytes of a group) do not fit, the scan is charged the spill of its partial states, as the normal plan is charged its own, and
-- the dearer of the two is not used: the refusal is `group_memory`, and only when the spill decided it (`cost` otherwise).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '30s';

CREATE TABLE facts (id int, u int, v int, w float8);
INSERT INTO facts SELECT i, i, i % 100, i * 0.5 FROM generate_series(1, 30000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, u, v, w);
VACUUM (FREEZE, ANALYZE) facts;

CREATE FUNCTION q(keys text) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT %s count(*) AS n, sum(v) AS s, avg(w) AS a, min(v) AS lo, max(w) AS hi FROM facts %s', coalesce(keys || ',', ''), CASE WHEN keys IS NULL THEN '' ELSE 'GROUP BY ' || keys END) $$;

-- the plan of a query and why it was or was not the scan, for a hash memory and a number of groups
CREATE FUNCTION why(keys text, mem text, mult text DEFAULT '1') RETURNS TABLE (hash_kb int, groups text, used boolean, slug text, detail boolean)
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('work_mem', mem, true), set_config('hash_mem_multiplier', mult, true);
	RETURN QUERY SELECT (pg_size_bytes(current_setting('work_mem')) / 1024 * current_setting('hash_mem_multiplier')::float8)::int,
		coalesce(keys, 'none'), e.used, e.slug, e.reason LIKE '%groups need % kB, a hash aggregate gets % kB%'
	FROM sift.explain_why(q(keys)) e;
END $$;

-- 30,000 groups of about 200 bytes fit the 8 MB of the defaults; they do not fit 1 MB, and the sort of the normal plan is the cheaper spill
SELECT * FROM why('u', '4MB', '2');
SELECT * FROM why('u', '1MB');
-- 100 groups, or one, fit any memory
SELECT * FROM why('v', '64kB');
SELECT * FROM why(NULL, '64kB');

-- the same rows, whichever plan the memory picks
SET sift.enable_agg = off;
CREATE TEMP TABLE want AS SELECT u, count(*) AS n, sum(v) AS s, avg(w) AS a, min(v) AS lo, max(w) AS hi FROM facts GROUP BY u;
RESET sift.enable_agg;
CREATE TEMP TABLE got_scan AS SELECT u, count(*) AS n, sum(v) AS s, avg(w) AS a, min(v) AS lo, max(w) AS hi FROM facts GROUP BY u;
SET work_mem = '1MB';
SET hash_mem_multiplier = 1;
CREATE TEMP TABLE got_normal AS SELECT u, count(*) AS n, sum(v) AS s, avg(w) AS a, min(v) AS lo, max(w) AS hi FROM facts GROUP BY u;
RESET work_mem;
RESET hash_mem_multiplier;
SELECT (SELECT count(*) FROM (TABLE want EXCEPT TABLE got_scan) d) AS missing_scan,
       (SELECT count(*) FROM (TABLE got_scan EXCEPT TABLE want) d) AS extra_scan,
       (SELECT count(*) FROM (TABLE want EXCEPT TABLE got_normal) d) AS missing_normal,
       (SELECT count(*) FROM want) AS groups;

DROP FUNCTION why(text, text, text), q(text);
DROP TABLE facts;
DROP EXTENSION pg_sift;
