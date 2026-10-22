-- sift.col_clustering reports, per integer leaf, how many units an equality or a range touches on the zone maps of the index, and so whether sorting
-- the table by the leaf would let the scans skip more. Its figures are those of Custom Scan (SiftAgg): the units read of a statement are checked against them.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

-- rows in id order, padded to about 1800 rows a unit; each leaf lays its values out differently
--   id: ascending     day: ascending in steps     blk: six blocks of consecutive rows, each in a range of its own
--   grp: 50 values in turn (every unit holds all)     rnd: scattered     lbl: text (no zones to report)
CREATE TABLE ev (id int, day date, blk int, grp int, rnd int, lbl text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, date '2020-01-01' + i / 300, (i / 10000) * 1000 + i % 100, i % 50, abs(hashint4(i)) % 100000, 'k' || i % 7, repeat('x', 1000)
FROM generate_series(0, 59999) i;
CREATE INDEX ev_col ON ev USING sift_col (id, day, blk, grp, rnd, lbl);
VACUUM (FREEZE, ANALYZE) ev;

SELECT leaf, path, units > 20 AS several_units, zoned_units = units AS all_zoned, round(useful_pct::numeric) AS useful_pct,
       round((point_units / units)::numeric, 2) AS point_share, round((range_units / units)::numeric, 2) AS range_share, round((range_ideal / units)::numeric, 2) AS ideal_share,
       round(correlation::numeric, 2) AS correlation, verdict
FROM sift.col_clustering('ev_col') ORDER BY leaf;

-- a wider window touches more units, and a window of the whole span about half of them from the middle of the data
SELECT path, round((range_units / units)::numeric, 2) AS share FROM sift.col_clustering('ev_col', 10) WHERE path IN ('id', 'blk') ORDER BY leaf;
SELECT path, range_units > units / 2 AS most_units FROM sift.col_clustering('ev_col', 100) WHERE path = 'id';

-- the units a statement reads: the average over values of the data is what point_units says (a quarter of the units of tolerance)
CREATE FUNCTION units_read(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '^\s*Units: ' THEN n := trim(split_part(l, ':', 2))::int; END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION predicted(col text, vals int[]) RETURNS TABLE (path text, matches bool) LANGUAGE plpgsql AS $$
DECLARE v int; total numeric := 0; avg_read numeric; pt float8; n int;
BEGIN
	SELECT c.point_units, c.units INTO pt, n FROM sift.col_clustering('ev_col') c WHERE c.path = col;
	FOREACH v IN ARRAY vals LOOP
		total := total + units_read(format('SELECT count(*) FROM ev WHERE %I = %s', col, v));
	END LOOP;
	avg_read := total / array_length(vals, 1);
	path := col;
	matches := abs(avg_read - pt) <= greatest(1, n * 0.25);
	RETURN NEXT;
END $$;

SELECT * FROM predicted('id', ARRAY[100, 9000, 31000, 52000]);
SELECT * FROM predicted('blk', ARRAY[5, 1005, 3042, 5099]);
SELECT * FROM predicted('grp', ARRAY[0, 7, 31]);

-- NULL leaves and errors
SELECT count(*) FILTER (WHERE zoned_units IS NULL) AS leaves_without_zones FROM sift.col_clustering('ev_col');
CREATE INDEX ev_bm25 ON ev USING sift (lbl);
SELECT * FROM sift.col_clustering('ev_bm25');
SELECT * FROM sift.col_clustering('ev_col', 101);
CREATE TABLE empty_t (id int);
CREATE INDEX empty_col ON empty_t USING sift_col (id);
SELECT leaf, path, units, zoned_units, verdict FROM sift.col_clustering('empty_col');

-- sift.cluster_advice: which order of the table would let the scans skip the most units, and when it is not worth it
-- ev: the leaves that are ordered now (id, day, blk) pay for the sort by rnd; evs: only scattered leaves; evo: ordered, small
CREATE TABLE evs (id int PRIMARY KEY, grp int, rnd int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO evs SELECT i, abs(hashint4(i)) % 300, abs(hashint4(i + 7)) % 100000, repeat('x', 1000) FROM generate_series(0, 59999) i;
CREATE INDEX evs_col ON evs USING sift_col (grp, rnd);
CREATE TABLE evo (id int, day date, pad text) WITH (autovacuum_enabled = false);
INSERT INTO evo SELECT i, date '2020-01-01' + i / 300, repeat('x', 1000) FROM generate_series(0, 59999) i;
CREATE INDEX evo_col ON evo USING sift_col (id, day);
VACUUM (FREEZE, ANALYZE) evs, evo;
SELECT tbl, key, round(units_now) AS now, round(units_after) AS after, verdict, split_part(reason, ':', 1) AS why, command IS NOT NULL AS has_command
FROM sift.cluster_advice('ev', 10000);
SELECT tbl, key, round(units_now) AS now, round(units_after) AS after, verdict, split_part(reason, ' reads', 1) AS why, command
FROM sift.cluster_advice('evs_col', 10000);
SELECT tbl, key, verdict, split_part(reason, ':', 1) AS why FROM sift.cluster_advice('evo', 10000);
SELECT tbl, key, verdict, split_part(reason, ' ', 1) AS why FROM sift.cluster_advice('evs');
-- the two switches
SET sift.cluster_advice = off;
SELECT tbl, key, verdict, reason FROM sift.cluster_advice('evs', 10000);
RESET sift.cluster_advice;
ALTER INDEX evs_col SET (cluster_advice = off);
SELECT tbl, key, verdict, reason FROM sift.cluster_advice('evs', 10000);
SELECT tbl, verdict FROM sift.cluster_advice('ev', 10000);
ALTER INDEX evs_col RESET (cluster_advice);
SELECT tbl, verdict FROM sift.cluster_advice('evs', 10000);
-- an index without a sealed unit has nothing to say, an index of another kind has no row
SELECT tbl, verdict, reason FROM sift.cluster_advice('empty_t');
SELECT count(*) AS rows FROM sift.cluster_advice('ev_bm25');

DROP TABLE ev, empty_t, evs, evo;
DROP FUNCTION units_read(text), predicted(text, int[]);
DROP EXTENSION pg_sift;
