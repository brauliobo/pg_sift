-- key_cut on a table of narrow rows: a superblock holds tens of thousands of such rows, and the units of the key cut are as large (up to 32,768 rows) with
-- the summaries of groups of units, so that a scan that does not use the key reads about the units of the page cut and answers from the same pre-aggregates,
-- and a lookup of the key still reads a unit or two. Four integer columns, 1,200,000 rows in a random order of the key.
CREATE EXTENSION pg_sift;
SET default_statistics_target = 10000;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE nu (id int, cust int, amt int, q int) WITH (autovacuum_enabled = false);
INSERT INTO nu SELECT i, ((i::bigint * 7919) % 20000)::int, ((i::bigint * 31) % 1000)::int, i % 100 FROM generate_series(1, 1200000) i;
CREATE TABLE nk (LIKE nu);
INSERT INTO nk SELECT * FROM nu;
VACUUM (FREEZE, ANALYZE) nu;
VACUUM (FREEZE, ANALYZE) nk;
CREATE INDEX nu_idx ON nu USING sift_col (id, cust, amt, q) WITH (dead_rewrite_pct = 10);
CREATE INDEX nk_idx ON nk USING sift_col (id, cust, amt, q) WITH (dead_rewrite_pct = 10, key_cut = 'cust');
SELECT sift.col_verify('nk_idx', true);

-- no more than twice the units of the page cut
SELECT (SELECT sum(units) FROM sift.col_segments('nk_idx') WHERE kind = 'immutable') <= 2 * (SELECT sum(units) FROM sift.col_segments('nu_idx') WHERE kind = 'immutable') AS units_within_twice;

CREATE FUNCTION nunits(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; u text := ''; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Units: %' THEN u := u || ' ' || trim(l); END IF;
	END LOOP;
	RETURN u;
END $$;

-- whole-table sum and a group by of a column the cube holds: every unit from the pre-aggregates, in both
SELECT 'page cut' AS index, nunits('SELECT sum(amt) FROM nu') AS sum_units UNION ALL SELECT 'key cut', nunits('SELECT sum(amt) FROM nk');
SELECT 'page cut' AS index, nunits('SELECT q, count(*), sum(amt) FROM nu GROUP BY 1') AS group_units UNION ALL SELECT 'key cut', nunits('SELECT q, count(*), sum(amt) FROM nk GROUP BY 1');

-- the same answers as the page cut and as the normal plan
SELECT (SELECT sum(amt) FROM nk) = (SELECT sum(amt) FROM nu) AS sum_same,
       NOT EXISTS (SELECT 1 FROM (SELECT q, count(*), sum(amt) FROM nk GROUP BY 1 EXCEPT SELECT q, count(*), sum(amt) FROM nu GROUP BY 1) d) AS group_same;

-- a lookup of the key reads one unit or two
SELECT nunits('SELECT count(*), sum(amt) FROM nk WHERE cust = 4242') AS lookup_units;
SELECT nunits('SELECT count(*), sum(amt) FROM nk WHERE cust IN (7, 9000, 19999)') AS list_units;
SELECT (SELECT count(*) || ' ' || sum(amt) FROM nk WHERE cust = 4242) = (SELECT count(*) || ' ' || sum(amt) FROM nu WHERE cust = 4242) AS lookup_same;

-- a few pages of an all-visible heap lose rows (the visibility map is read 8 bytes of 32 pages at a time, the pages are in different words): the keys of those rows
-- are counted without them, from the units whose pages are not all-visible
CREATE TEMP TABLE dirty_ids AS SELECT id FROM nu WHERE ctid >= '(40,1)' AND ctid < '(41,1)' OR ctid >= '(2500,1)' AND ctid < '(2501,1)' OR ctid >= '(5000,1)' AND ctid < '(5001,1)';
CREATE TEMP TABLE dirty_custs AS SELECT DISTINCT cust FROM nu WHERE id IN (SELECT id FROM dirty_ids);
DELETE FROM nk WHERE id IN (SELECT id FROM dirty_ids);
DELETE FROM nu WHERE id IN (SELECT id FROM dirty_ids);
SELECT (SELECT count(*) FROM dirty_ids) > 100 AS rows_deleted,
       NOT EXISTS (SELECT 1 FROM (SELECT cust, count(*), sum(amt) FROM nk WHERE cust IN (SELECT cust FROM dirty_custs) GROUP BY 1 EXCEPT SELECT cust, count(*), sum(amt) FROM nu WHERE cust IN (SELECT cust FROM dirty_custs) GROUP BY 1) d) AS dirty_pages_same;
DROP TABLE dirty_ids, dirty_custs;

-- after deletes and a merge the units are cut again and the answers hold
DELETE FROM nk WHERE id % 11 = 0;
DELETE FROM nu WHERE id % 11 = 0;
-- a checkpoint writing the pages the deletes dirtied pins them, and VACUUM skips a page it cannot get the cleanup lock of and leaves it not all-visible
CHECKPOINT;
VACUUM nk; VACUUM nu;

-- units with dead rows over an all-visible heap are read without the heap: no row is vetted
CREATE FUNCTION nvetted(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Rows: %' THEN RETURN trim(l); END IF;
	END LOOP;
	RETURN 'no agg';
END $$;
SELECT nvetted('SELECT q, count(*), sum(amt) FROM nk GROUP BY 1') AS dirty_vetted;
SELECT nvetted('SELECT sum(amt) FROM nk WHERE amt > 0') AS dirty_filter_vetted;
DROP FUNCTION nvetted(text);

SELECT sift.col_merge('nk_idx', true) >= 0 AS merged;
SELECT sift.col_verify('nk_idx', true);
SELECT (SELECT sum(amt) FROM nk) = (SELECT sum(amt) FROM nu) AS sum_same,
       (SELECT count(*) || ' ' || sum(amt) FROM nk WHERE cust = 4242) = (SELECT count(*) || ' ' || sum(amt) FROM nu WHERE cust = 4242) AS lookup_same;
SELECT nunits('SELECT sum(amt) FROM nk') AS sum_units_after;

DROP FUNCTION nunits(text);
DROP TABLE nu, nk;
DROP EXTENSION pg_sift;
