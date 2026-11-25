-- The workers of a scan over a partitioned table get the superblock offsets of the partitions from the leader and used to open the first
-- partition only: a worker with an item in another partition ran on a source that was not there. Each query runs with workers, with and without
-- the leader scanning too, and gives the rows of the scan without workers and of the normal plan; for hash, range and list partitions.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.agg_preagg = off;

CREATE TABLE ph (id int, grp int, kind text, ts date, amount int, val float8) PARTITION BY HASH (id);
CREATE TABLE pr (id int, grp int, kind text, ts date, amount int, val float8) PARTITION BY RANGE (id);
CREATE TABLE pl (id int, grp int, kind text, ts date, amount int, val float8) PARTITION BY LIST (grp);
DO $$
BEGIN
	FOR i IN 0..3 LOOP
		EXECUTE format('CREATE TABLE ph_%s PARTITION OF ph FOR VALUES WITH (MODULUS 4, REMAINDER %s) WITH (fillfactor = 10, autovacuum_enabled = false)', i, i);
		EXECUTE format('CREATE TABLE pr_%s PARTITION OF pr FOR VALUES FROM (%s) TO (%s) WITH (fillfactor = 10, autovacuum_enabled = false)', i, i * 1500 + 1, i * 1500 + 1501);
		EXECUTE format('CREATE TABLE pl_%s PARTITION OF pl FOR VALUES IN (%s, %s, %s) WITH (fillfactor = 10, autovacuum_enabled = false)', i, i * 3, i * 3 + 1, i * 3 + 2);
	END LOOP;
END $$;
CREATE TABLE pl_none PARTITION OF pl DEFAULT WITH (fillfactor = 10, autovacuum_enabled = false);

-- every table has the same rows; NULLs and duplicates among the values
INSERT INTO ph SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 12 END, CASE WHEN i % 13 = 0 THEN NULL ELSE 'k' || i % 5 END, date '2026-01-01' + i % 90,
                      CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7) % 1000 END, CASE WHEN i % 19 = 0 THEN NULL ELSE ((i * 13) % 5000) / 8.0 END
FROM generate_series(1, 6000) i;
INSERT INTO pr SELECT * FROM ph;
INSERT INTO pl SELECT * FROM ph;
CREATE INDEX ph_col ON ph USING sift_col (id, grp, kind, ts, amount, val);
CREATE INDEX pr_col ON pr USING sift_col (id, grp, kind, ts, amount, val);
CREATE INDEX pl_col ON pl USING sift_col (id, grp, kind, ts, amount, val);
VACUUM (FREEZE, ANALYZE) ph, pr, pl;

CREATE FUNCTION p_rows(q text, push bool, workers int, leader bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	PERFORM set_config('parallel_leader_participation', leader::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION p_mode(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_mode_two_workers(q) $$;

-- the workers of the scan started (a launch can fail when the other sessions hold the workers: tried again)
CREATE FUNCTION p_launched(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; launched bool;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	PERFORM set_config('parallel_leader_participation', 'off', true);
	FOR i IN 1..20 LOOP
		launched := false;
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			launched := launched OR l ~ 'Workers Launched: [1-9]';
		END LOOP;
		IF launched THEN RETURN true; END IF;
		PERFORM pg_sleep(0.2);
	END LOOP;
	RETURN false;
END $$;

-- %1$s: the table. The floats are rounded: the normal plan adds them in another order
CREATE TABLE p_q (n serial, q text);
INSERT INTO p_q (q) VALUES
	('SELECT max(val), min(val), max(amount), min(ts) FROM %1$s'),
	('SELECT grp, max(amount), min(val), max(ts) FROM %1$s GROUP BY 1'),
	('SELECT kind, count(*), count(amount), sum(amount), round(avg(val)::numeric, 6) FROM %1$s GROUP BY 1'),
	('SELECT grp, count(DISTINCT kind), count(DISTINCT amount) FROM %1$s GROUP BY 1'),
	('SELECT count(DISTINCT grp), count(DISTINCT ts), round(sum(val)::numeric, 3) FROM %1$s'),
	('SELECT grp, string_agg(DISTINCT kind, '','' ORDER BY kind), array_agg(DISTINCT amount %% 7 ORDER BY amount %% 7) FROM %1$s GROUP BY 1'),
	('SELECT kind, grp, max(val), count(*) FROM %1$s WHERE amount < 600 AND ts >= date ''2026-02-01'' GROUP BY 1, 2');

CREATE FUNCTION p_compare(OUT tbl text, OUT n int, OUT mode text, OUT leader_off bool, OUT leader_on bool, OUT one bool, OUT four bool) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE q text; plain text;
BEGIN
	FOREACH tbl IN ARRAY ARRAY['ph', 'pr', 'pl'] LOOP
		FOR n, q IN SELECT p_q.n, format(p_q.q, tbl) FROM p_q ORDER BY p_q.n LOOP
			mode := p_mode(q);
			plain := p_rows(q, false, 0, true);
			leader_off := p_rows(q, true, 2, false) IS NOT DISTINCT FROM plain;
			leader_on := p_rows(q, true, 2, true) IS NOT DISTINCT FROM plain;
			one := p_rows(q, true, 1, false) IS NOT DISTINCT FROM plain;
			four := p_rows(q, true, 4, false) IS NOT DISTINCT FROM plain;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;

SELECT * FROM p_compare();

-- the workers started, and read partitions the first one is not
SELECT tbl, p_launched(format(q, tbl)) AS launched FROM p_q, unnest(ARRAY['ph', 'pr', 'pl']) tbl WHERE n IN (1, 2) ORDER BY tbl, n;

-- rows in the write buffers and rows updated and deleted since the index was built
INSERT INTO ph SELECT i, i % 12, 'k' || i % 5, date '2026-04-01' + i % 20, i % 300, (i % 400) / 8.0 FROM generate_series(6001, 6400) i;
INSERT INTO pr SELECT * FROM ph WHERE id > 6000;
INSERT INTO pl SELECT * FROM ph WHERE id > 6000;
UPDATE ph SET amount = amount + 1000, val = val + 1 WHERE id % 97 = 0;
UPDATE pr SET amount = amount + 1000, val = val + 1 WHERE id % 97 = 0;
UPDATE pl SET amount = amount + 1000, val = val + 1 WHERE id % 97 = 0;
DELETE FROM ph WHERE id % 89 = 0;
DELETE FROM pr WHERE id % 89 = 0;
DELETE FROM pl WHERE id % 89 = 0;
SELECT * FROM p_compare();

-- the limit on the partitions of a scan (lowered): ph and pr have 4, pl 5 (with the default one). Over it the table keeps the normal plan, at it the
-- scan has its workers; the rows are the same
CREATE TABLE p_lim (lim int);
INSERT INTO p_lim VALUES (3), (4), (5);
CREATE FUNCTION p_limit(tbl text, lim int, OUT mode text, OUT same_rows bool) LANGUAGE plpgsql AS $$
DECLARE q text := format((SELECT q FROM p_q WHERE n = 2), tbl);
BEGIN
	PERFORM set_config('sift.test_limits', 'partitions=' || lim, true);
	mode := p_mode(q);
	same_rows := p_rows(q, true, 2, false) IS NOT DISTINCT FROM p_rows(q, false, 0, true);
END $$;
SELECT tbl, lim, (p_limit(tbl, lim)).* FROM unnest(ARRAY['ph', 'pr', 'pl']) tbl, p_lim ORDER BY tbl, lim;

DROP TABLE ph, pr, pl, p_q, p_lim;
DROP FUNCTION p_limit(text, int);
DROP FUNCTION p_rows(text, bool, int, bool), p_mode(text), p_launched(text), p_compare();
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
