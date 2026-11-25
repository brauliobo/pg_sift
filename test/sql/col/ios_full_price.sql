-- The rows of 100 keys joined to a wide table (40 integer columns) that has a sift_col index and no b-tree: the hash join over a full index-only scan of the 20,000 rows
-- was priced 600 against 790 for the scan of the heap, and takes 8 ms where the heap takes 0.8. An index-only scan forms a tuple of the columns for each row it gives,
-- 0.21 us measured (the TID of the bitmap node takes 10 ns), where it was priced 3 ns.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

DO $$
DECLARE cols text := ''; defs text := ''; i int;
BEGIN
	FOR i IN 1..40 LOOP
		cols := cols || format(', (g * %s %% 1000)::int AS c%s', i + 1, i);
		IF i <= 20 THEN defs := defs || format(', c%s', i); END IF;
	END LOOP;
	EXECUTE 'CREATE TABLE wide AS SELECT (g::bigint * 7919 % 20011)::int AS k, g AS n' || cols || ' FROM generate_series(1, 20000) g';
	EXECUTE 'CREATE INDEX wide_c ON wide USING sift_col (k, n' || defs || ')';
END $$;
CREATE TABLE kk AS SELECT (i::bigint * 7919 % 20011)::int AS k FROM generate_series(1, 100) i;
VACUUM (FREEZE, ANALYZE) wide, kk;

CREATE FUNCTION scan_of_wide(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Scan' AND l ~ 'wide' THEN RETURN regexp_replace(l, '^\s*(->\s*)?(.*) on wide.*', '\2'); END IF;
	END LOOP;
END $$;

SELECT scan_of_wide('SELECT t.k, t.n FROM kk JOIN wide t ON t.k = kk.k ORDER BY t.n') AS join_scan;
SELECT scan_of_wide('SELECT t.k, t.n FROM wide t WHERE t.k < 50') AS selective_scan;

DROP FUNCTION scan_of_wide(text);
DROP TABLE wide, kk;
DROP EXTENSION pg_sift;
