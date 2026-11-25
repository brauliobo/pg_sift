-- Format 2.0 sift_table: the rows a statement reads by address, and the quals it hands to the scan, give the rows a heap gives.
-- The key order scatters the addresses of the rows over the granules of the part (no granule holds a range of them), as the load of a table in the order of a key does.
-- About 3 s. EXPLAIN (SIFT) needs PostgreSQL 18: table_f2_fetch_1.out is the same test where it says so. Run on a build where new tables take Format 2.0 (sift.test_create_format1 off).
CREATE EXTENSION IF NOT EXISTS pg_sift CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '240s';

CREATE TABLE h (id bigint PRIMARY KEY, cust int NOT NULL, ts timestamptz NOT NULL, amt numeric(10,2) NOT NULL, tag text NOT NULL);
INSERT INTO h SELECT g, 1 + floor(sqrt((g * 7919) % 160000))::int, timestamptz '2024-01-01' + g * interval '1 minute', (g % 1000) / 4.0, 'tag' || g % 50 FROM generate_series(1, 200000) g;
CREATE TABLE t (LIKE h) USING sift_table;
ALTER TABLE t ADD PRIMARY KEY (id);
SELECT sift.table_set_option('t', 'key_order', 'cust, ts, id');
INSERT INTO t SELECT * FROM h;
SELECT sift.create_projection('t', 'p_cust', 'ORDER BY cust, ts DESC, id DESC INCLUDE (amt)') AS projection;
-- a compaction writes a part for each quarter of maintenance_work_mem of rows: this one makes a single part of the table
SET maintenance_work_mem = '256MB';
VACUUM (FREEZE, ANALYZE) t;
RESET maintenance_work_mem;
ANALYZE h;

-- the rows are told apart by their granule: the addresses of a granule spread over the table
SELECT max(granules) > 16 AS many_granules FROM sift.parts('t'::regclass);

-- 1. rows fetched by address through the primary key, in the order of the address and not of the key
CREATE FUNCTION rows_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(x::text, ''|'' ORDER BY x::text)) FROM (' || q || ') x' INTO r;
	RETURN r;
END $$;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET sift.enable_agg = off;
SELECT rows_of('SELECT * FROM t WHERE id = ANY(ARRAY(SELECT g * 37 FROM generate_series(1, 5000) g))') =
       rows_of('SELECT * FROM h WHERE id = ANY(ARRAY(SELECT g * 37 FROM generate_series(1, 5000) g))') AS fetch_by_key_parity;
SELECT rows_of('SELECT * FROM t WHERE id BETWEEN 100000 AND 100400') = rows_of('SELECT * FROM h WHERE id BETWEEN 100000 AND 100400') AS fetch_range_parity;
SELECT rows_of('SELECT * FROM t WHERE id IN (1, 200000, 77777, 123456, 199999, 2)') = rows_of('SELECT * FROM h WHERE id IN (1, 200000, 77777, 123456, 199999, 2)') AS fetch_edges_parity;
SET sift.backend_cache_kb = 0;
SELECT rows_of('SELECT * FROM t WHERE id = ANY(ARRAY(SELECT g * 37 FROM generate_series(1, 500) g))') =
       rows_of('SELECT * FROM h WHERE id = ANY(ARRAY(SELECT g * 37 FROM generate_series(1, 500) g))') AS fetch_without_cache_parity;
RESET sift.backend_cache_kb;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET sift.enable_agg;

-- 2. a correlated aggregate: the scan takes the outer value at each rescan (the quals of the scan hold a parameter)
CREATE FUNCTION plan_nodes(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan' THEN RETURN NEXT btrim(regexp_replace(l, '\s+on\s+\w+', '')); END IF;
	END LOOP;
END $$;

SELECT plan_nodes('SELECT c, (SELECT count(*) FROM t o WHERE o.cust = c) FROM generate_series(1, 30) c');
SELECT rows_of('SELECT c, (SELECT count(*) FROM t o WHERE o.cust = c), (SELECT sum(amt) FROM t o WHERE o.cust = c) FROM generate_series(1, 30) c') =
       rows_of('SELECT c, (SELECT count(*) FROM h o WHERE o.cust = c), (SELECT sum(amt) FROM h o WHERE o.cust = c) FROM generate_series(1, 30) c') AS correlated_parity;
SELECT count(DISTINCT n) > 1 AS counts_follow_outer FROM (SELECT (SELECT count(*) FROM t o WHERE o.cust = c) AS n FROM generate_series(1, 30) c) x;
SELECT c, (SELECT count(*) FROM t o WHERE o.cust = c) AS n FROM (VALUES (NULL::int), (5), (NULL), (6)) v(c) ORDER BY c NULLS FIRST, 2;

-- the parameter of a prepared statement, taken as a generic plan
SET plan_cache_mode = force_generic_plan;
PREPARE pc(int) AS SELECT count(*), sum(amt) FROM t WHERE cust = $1;
PREPARE ph(int) AS SELECT count(*), sum(amt) FROM h WHERE cust = $1;
CREATE FUNCTION prepared_parity(n int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE c int; t_n bigint; t_s numeric; h_n bigint; h_s numeric; ok bool := true;
BEGIN
	FOR c IN 1..n LOOP
		EXECUTE format('EXECUTE pc(%s)', c) INTO t_n, t_s;
		EXECUTE format('EXECUTE ph(%s)', c) INTO h_n, h_s;
		ok := ok AND t_n = h_n AND t_s IS NOT DISTINCT FROM h_s;
	END LOOP;
	RETURN ok;
END $$;
SELECT prepared_parity(40) AS generic_plan_parity;
EXECUTE pc(NULL);
RESET plan_cache_mode;

-- 3. a keyset page in descending order of the projection (a bounded sort of the rows that satisfy the quals)
SELECT plan_nodes('SELECT id, ts, amt FROM t WHERE cust = 12 AND (ts, id) < (timestamptz ''2024-02-01'', 150000) ORDER BY ts DESC, id DESC LIMIT 25');
SELECT rows_of('SELECT id, ts, amt FROM t WHERE cust = 12 AND (ts, id) < (timestamptz ''2024-02-01'', 150000) ORDER BY ts DESC, id DESC LIMIT 25') =
       rows_of('SELECT id, ts, amt FROM h WHERE cust = 12 AND (ts, id) < (timestamptz ''2024-02-01'', 150000) ORDER BY ts DESC, id DESC LIMIT 25') AS keyset_desc_parity;
SELECT count(*) AS page_rows FROM (SELECT id FROM t WHERE cust = 12 ORDER BY ts DESC, id DESC LIMIT 25) p;
SELECT rows_of('SELECT id, ts FROM t WHERE cust = 12 ORDER BY ts DESC, id DESC LIMIT 7') = rows_of('SELECT id, ts FROM h WHERE cust = 12 ORDER BY ts DESC, id DESC LIMIT 7') AS page_parity;

-- 4. EXPLAIN (SIFT) on a Format 2.0 index that a plan reads with an index scan
CREATE TABLE o (id int PRIMARY KEY, c int NOT NULL, v float8 NOT NULL);
INSERT INTO o SELECT i, i % 50, i FROM generate_series(1, 5000) i;
CREATE INDEX o_col ON o USING sift_col (id, c, v);
VACUUM (ANALYZE) o;
CREATE FUNCTION sift_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (SIFT, COSTS OFF) ' || q LOOP
		IF l ~ '(Index.*Scan|Sift Index State)' THEN RETURN NEXT regexp_replace(btrim(l), '[0-9]+ segments', 'N segments'); END IF;
	END LOOP;
EXCEPTION WHEN syntax_error THEN
	RETURN NEXT 'EXPLAIN (SIFT) needs PostgreSQL 18';
END $$;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET sift.enable_agg = off;
SELECT sift_lines('SELECT id FROM o WHERE c = 7 AND v > 100');
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET sift.enable_agg;

DROP FUNCTION rows_of(text), plan_nodes(text), prepared_parity(int), sift_lines(text);
DEALLOCATE ALL;
DROP TABLE t, h, o;
DROP EXTENSION pg_sift CASCADE;
