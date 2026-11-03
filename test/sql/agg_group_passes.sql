-- A scan whose table would outgrow hash memory is read in passes (agg/spill/pass.c), each for the groups whose key hashes to its share: the answers are the
-- normal plan's, for every kind of key, with NULL keys and values, a group much larger than the others, shares with no row, and a pass that outgrows hash
-- memory itself (work_mem 64kB, hash_mem_multiplier 1) and spills.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '120s';

CREATE TABLE ev (id int, ki int, kt text, kd date, kts timestamp, kv varchar, u int, v int, w numeric(10,2));
INSERT INTO ev
SELECT i, CASE WHEN i % 17 = 0 THEN NULL WHEN i % 10 = 0 THEN 7 ELSE (i * 7919) % 20000 END,
       CASE WHEN i % 19 = 0 THEN NULL ELSE 'k' || (i * 31) % 15000 END, CASE WHEN i % 23 = 0 THEN NULL ELSE date '2020-01-01' + (i * 13) % 3000 END,
       timestamp '2020-01-01' + ((i * 17) % 90000) * interval '1 minute', 'v' || (i * 7) % 9000,
       CASE WHEN i % 13 = 0 THEN NULL ELSE (i::bigint * 104729) % 40000 END, i % 1000, (i % 313) / 4.0
FROM generate_series(1, 120000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, ki, kt, kd, kts, kv, u, v, w);
VACUUM (FREEZE, ANALYZE) ev;
-- a second segment (new strings in its dictionaries), a write buffer, and dead rows and new row versions: one statement reads one snapshot, so every pass
-- sees the same rows
INSERT INTO ev SELECT i, (i * 7919) % 25000, 'k' || (i * 31) % 20000, date '2021-01-01' + (i * 13) % 500, timestamp '2021-01-01' + (i % 700) * interval '1 hour',
       'v' || (i * 7) % 12000, (i::bigint * 104729) % 50000, i % 1000, (i % 313) / 4.0 FROM generate_series(120001, 140000) i;
SELECT sift.col_promote('ev_col') IS NOT NULL AS promoted;
INSERT INTO ev SELECT i, (i * 7919) % 30000, 'k' || (i * 31) % 22000, NULL, timestamp '2022-01-01', 'v' || i % 10, NULL, i % 1000, 1.5 FROM generate_series(140001, 150000) i;
UPDATE ev SET u = u + 1 WHERE id % 29 = 0;
DELETE FROM ev WHERE id % 31 = 0;

CREATE TABLE qs (name text PRIMARY KEY, q text);
INSERT INTO qs VALUES
	('int key', 'SELECT ki, count(*), count(DISTINCT u), sum(v), min(v), max(v), avg(w) FROM ev GROUP BY ki'),
	('text key', 'SELECT kt, count(*), count(DISTINCT u), sum(v) FROM ev GROUP BY kt'),
	('varchar key', 'SELECT kv, count(*), count(DISTINCT u) FROM ev GROUP BY kv'),
	('date key', 'SELECT kd, count(*), count(DISTINCT u), max(w) FROM ev GROUP BY kd'),
	('timestamp key', 'SELECT kts, count(*), count(DISTINCT u), min(v) FROM ev GROUP BY kts'),
	('two keys', 'SELECT ki, kt, count(*), count(DISTINCT u) FROM ev GROUP BY ki, kt'),
	('filtered distinct', 'SELECT kt, count(DISTINCT u) FILTER (WHERE v > 500), count(DISTINCT v) FROM ev GROUP BY kt'),
	('few rows, most shares empty', 'SELECT ki, count(*), count(DISTINCT u) FROM ev WHERE id < 40 GROUP BY ki'),
	('no row', 'SELECT kt, count(*), count(DISTINCT u) FROM ev WHERE id < 0 GROUP BY kt'),
	('top-n', 'SELECT * FROM (SELECT ki, count(DISTINCT u) c FROM ev GROUP BY ki ORDER BY c DESC, ki LIMIT 20) t'),
	('having', 'SELECT kt, count(DISTINCT u) c FROM ev GROUP BY kt HAVING count(DISTINCT u) > 6');

CREATE FUNCTION digest(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE d text;
BEGIN
	EXECUTE format($f$SELECT md5(coalesce(string_agg(t::text, E'\n' ORDER BY t::text), '')) FROM (%s) t$f$, q) INTO d;
	RETURN d;
END $$;

-- the temporary blocks a statement wrote, from its plan (the scan's own)
CREATE FUNCTION temp_written(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, BUFFERS ON) ' || q INTO j;
	RETURN coalesce(jsonb_path_query_first(j, '$.**."Temp Written Blocks"')::bigint, 0);
END $$;

-- the passes a statement made, from its plan
CREATE FUNCTION passes(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, BUFFERS OFF) ' || q INTO j;
	RETURN coalesce(jsonb_path_query_first(j, '$.**."Group Passes"')::int, 0);
END $$;

SET sift.enable_agg = off;
CREATE TABLE want AS SELECT name, digest(q) AS d FROM qs;
RESET sift.enable_agg;

-- one share a pass, three shares a pass, the whole table in the memory it asks for
SELECT s AS shares_a_pass, name, digest(q) = d AS same, passes(q) > 0 AS in_passes
FROM unnest(ARRAY[1, 3]) s, LATERAL (SELECT set_config('sift.test_agg_pass_w', s::text, false)) c, qs JOIN want USING (name)
ORDER BY 1, 2;
SET sift.test_agg_pass_w = 0;

-- a pass that does not fit hash memory spills, and its partitions are merged when it ends
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT s AS shares_a_pass, name, digest(q) = d AS same
FROM unnest(ARRAY[1, 4]) s, LATERAL (SELECT set_config('sift.test_agg_pass_w', s::text, false)) c, qs JOIN want USING (name)
ORDER BY 1, 2;
SET sift.test_agg_pass_w = 0;

-- the memory alone decides: the scan that outgrows it early starts again in passes (and a pass that outgrows it again is read with fewer shares); at 64kB
-- the estimate says that not one share fits, and the scan spills as before
SELECT name, digest(q) = d AS same, passes(q) > 0 AS in_passes
FROM qs JOIN want USING (name) WHERE name IN ('int key', 'top-n')
ORDER BY 1;
SET work_mem = '1MB';
SELECT name, digest(q) = d AS same, passes(q) > 0 AS in_passes
FROM qs JOIN want USING (name) WHERE name IN ('int key', 'top-n')
ORDER BY 1;
SET sift.test_agg_pass_w = -1;
SELECT name, passes(q) AS passes FROM qs WHERE name IN ('int key', 'top-n') ORDER BY 1;
SET sift.test_agg_pass_w = 0;
-- one share does not fit: the shares left are read together and spill, not a pass for each of the 32
SELECT name, passes(q) < 32 AS not_a_pass_a_share FROM qs WHERE name IN ('int key', 'two keys', 'text key') ORDER BY 1;
RESET sift.test_agg_pass_w;
RESET work_mem;
RESET hash_mem_multiplier;

-- the temporary files: the scan that spilled writes none in passes
SET work_mem = '2MB';
SET hash_mem_multiplier = 1;
SET sift.test_agg_pass_w = -1;
SELECT passes(q) AS passes, temp_written(q) > 0 AS wrote_temp FROM (SELECT 'SELECT * FROM (SELECT kt, count(DISTINCT u) c FROM ev GROUP BY kt ORDER BY c DESC, kt LIMIT 20) t' AS q) x;
SET sift.test_agg_pass_w = 0;
SELECT passes(q) > 0 AS in_passes, temp_written(q) > 0 AS wrote_temp FROM (SELECT 'SELECT * FROM (SELECT kt, count(DISTINCT u) c FROM ev GROUP BY kt ORDER BY c DESC, kt LIMIT 20) t' AS q) x;
RESET sift.test_agg_pass_w;
RESET work_mem;
RESET hash_mem_multiplier;

-- a node read again replays its groups
SET sift.test_agg_pass_w = 2;
SELECT count(*) AS rows_replayed
FROM generate_series(1, 3) g, (SELECT kt, count(DISTINCT u) c FROM ev GROUP BY kt HAVING count(DISTINCT u) > 6) l;
SET work_mem = '1MB';
SET hash_mem_multiplier = 1;
SET sift.test_agg_pass_w = 0;
SELECT count(*) AS rows_replayed_after_restart
FROM generate_series(1, 3) g, (SELECT kt, count(DISTINCT u) c FROM ev GROUP BY kt HAVING count(DISTINCT u) > 6) l;
RESET work_mem;
RESET hash_mem_multiplier;
SET sift.enable_agg = off;
SELECT count(*) AS rows_replayed_normal
FROM generate_series(1, 3) g, (SELECT kt, count(DISTINCT u) c FROM ev GROUP BY kt HAVING count(DISTINCT u) > 6) l;
RESET sift.enable_agg;
RESET sift.test_agg_pass_w;

DROP TABLE ev, qs, want;
DROP FUNCTION digest(text), passes(text), temp_written(text);
DROP EXTENSION pg_sift;
