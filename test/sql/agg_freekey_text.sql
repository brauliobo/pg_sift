-- A text test on a member of a free-key object (`m.* text`, m->>'k1' = 'v4') runs in the aggregate pushdown: the rows are the rows of the
-- normal plan, also with workers.
CREATE EXTENSION pg_sift;

CREATE TABLE k_t (id int, m jsonb) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO k_t SELECT i, jsonb_build_object('k1', 'v' || i % 10, 'k2', 'w' || i % 5) FROM generate_series(1, 6000) i;
CREATE INDEX k_col ON k_t USING sift_col (id, m) WITH (paths = 'm.* text');
VACUUM ANALYZE k_t;

CREATE FUNCTION k_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE TABLE k_q (n serial, q text);
INSERT INTO k_q (q) VALUES
	('SELECT count(*) FROM k_t WHERE m->>''k1'' = ''v4'''),
	('SELECT count(*) FROM k_t WHERE m->>''k1'' IN (''v4'', ''v7'')'),
	('SELECT count(*) FROM k_t WHERE m->>''k1'' LIKE ''v4%'''),
	('SELECT m->>''k2'', count(*) FROM k_t WHERE m->>''k1'' > ''v4'' GROUP BY 1');

CREATE FUNCTION k_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE FUNCTION k_compare(OUT n int, OUT pushed bool, OUT serial_same bool, OUT parallel_same bool) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE q text;
BEGIN
	FOR n, q IN SELECT k_q.n, k_q.q FROM k_q ORDER BY k_q.n LOOP
		SET LOCAL max_parallel_workers_per_gather = 0;
		pushed := k_pushed(q);
		serial_same := k_rows(q, true) IS NOT DISTINCT FROM k_rows(q, false);
		SET LOCAL parallel_setup_cost = 0;
		SET LOCAL parallel_tuple_cost = 0;
		SET LOCAL min_parallel_table_scan_size = 0;
		SET LOCAL max_parallel_workers_per_gather = 2;
		parallel_same := k_rows(q, true) IS NOT DISTINCT FROM k_rows(q, false);
		RETURN NEXT;
	END LOOP;
END $$;

SET seq_page_cost = 10000;
SELECT n, pushed, serial_same, parallel_same FROM k_compare();

DROP TABLE k_t, k_q;
DROP FUNCTION k_rows(text, bool), k_pushed(text), k_compare();
DROP EXTENSION pg_sift;
