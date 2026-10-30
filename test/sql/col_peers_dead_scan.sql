-- The peers a parallel key_cut build leaves are keyed segments that overlap, number their units from 0 and differ in unit count; VACUUM keeps them and marks their units
-- dirty. The listing of an API (an IN list on the key, a range, ORDER BY .. LIMIT .. OFFSET) is read by the plain index scan (col_scan_begin -> sift_col_next ->
-- col_scan_unit -> sift_col_sel_all; a column the index does not hold keeps the plan off the covering paths): from one backend that stays connected through the peers with dead rows, a REINDEX that replaces them and the rewrite, every answer is
-- the one of the sequential scan, with a fresh IN list each time (what the backend keeps for a segment must not leak to another).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 4;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE rv (id int, prop int, d date, upd timestamp, del timestamp, note text) WITH (autovacuum_enabled = false);
INSERT INTO rv SELECT i, ((i::bigint * 7919) % 5000)::int, date '2020-01-01' + i / 400, timestamp '2020-01-01' + i * interval '40 seconds',
	CASE WHEN i % 17 = 0 THEN timestamp '2024-01-01' + i * interval '1 minute' END, 'n' FROM generate_series(1, 250000) i;
VACUUM (FREEZE, ANALYZE) rv;
CREATE INDEX rv_idx ON rv USING sift_col (id, prop, d, upd, del) WITH (key_cut = 'prop', key_cut_rows = 100, point_tid_columns = 'id, upd');

CREATE FUNCTION rv_peers() RETURNS TABLE (peers_overlap boolean, unequal_units boolean, dirty_units boolean) LANGUAGE sql AS
$$ SELECT count(*) > 1 AND count(DISTINCT sb_lo) = 1, count(DISTINCT units) > 1, coalesce(bool_or(dirty), false) FROM sift.col_segments('rv_idx') WHERE kind = 'immutable' $$;

-- the answer of the index scan against the sequential scan's, for the listing of dates, of the update time, or of the deleted rows
CREATE FUNCTION rv_chk(n int, k int, lim int, off int, shape int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE props int[] := ARRAY(SELECT ((g::bigint * 7919 + k::bigint * 104729) % 5000)::int FROM generate_series(1, n) g);
		q text; pl text; used bool := false; a int[]; b int[];
BEGIN
	q := CASE shape
		WHEN 1 THEN format('SELECT id, note FROM rv WHERE prop = ANY (%L) AND d BETWEEN %L AND %L ORDER BY d DESC, id LIMIT %s OFFSET %s', props, date '2020-02-01' + k * 7, date '2020-08-01' + k * 40, lim, off)
		WHEN 2 THEN format('SELECT id, note FROM rv WHERE prop = ANY (%L) AND upd > %L ORDER BY upd DESC, id LIMIT %s OFFSET %s', props, timestamp '2020-02-01' + k * interval '9 days', lim, off)
		ELSE format('SELECT id, note FROM rv WHERE prop = ANY (%L) AND del IS NOT NULL AND del::date BETWEEN %L AND %L ORDER BY del DESC, id LIMIT %s OFFSET %s', props, date '2024-01-01' + k * 5, date '2025-06-01', lim, off)
	END;
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = on; SET LOCAL sift.enable_agg = off; SET LOCAL seq_page_cost = 100;
	FOR pl IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP used := used OR pl LIKE '%Index Scan using rv_idx%'; END LOOP;
	EXECUTE format('SELECT array_agg(id) FROM (%s) r', q) INTO a;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL seq_page_cost = 1;
	EXECUTE format('SELECT array_agg(id) FROM (%s) r', q) INTO b;
	RETURN CASE WHEN a IS DISTINCT FROM b THEN format('DIFFERS shape %s n %s k %s lim %s off %s', shape, n, k, lim, off)
				WHEN NOT used THEN 'ok, but not the index scan' ELSE 'ok' END;
END $$;
CREATE FUNCTION rv_all() RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT rv_chk(n, k, lim, off, shape), 1::bigint FROM unnest(ARRAY[50, 250]) n, generate_series(1, 3) k, unnest(ARRAY[25, 100]) lim, unnest(ARRAY[0, 400]) off, generate_series(1, 3) shape $$;

-- 1. the peers as the build leaves them, then with dead rows in every one
SELECT * FROM rv_peers();
SELECT result, count(*) FROM rv_all() GROUP BY 1 ORDER BY 1;
DELETE FROM rv WHERE id % 13 = 0;
UPDATE rv SET upd = upd + interval '1 day' WHERE id % 29 = 0;
VACUUM rv;
ANALYZE rv;
SELECT * FROM rv_peers();
SELECT result, count(*) FROM rv_all() GROUP BY 1 ORDER BY 1;
-- 2. the peers are replaced under the connected backend, and the new ones get dead rows
REINDEX INDEX rv_idx;
SELECT * FROM rv_peers();
SELECT result, count(*) FROM rv_all() GROUP BY 1 ORDER BY 1;
DELETE FROM rv WHERE id % 11 = 0;
VACUUM rv;
ANALYZE rv;
SELECT * FROM rv_peers();
SELECT result, count(*) FROM rv_all() GROUP BY 1 ORDER BY 1;
-- 3. the rewrite replaces the peers under the same backend
SELECT sift.col_maintain('rv_idx') AS rewrote;
SELECT result, count(*) FROM rv_all() GROUP BY 1 ORDER BY 1;
SELECT sift.col_verify('rv_idx', true);

DROP TABLE rv CASCADE;
DROP FUNCTION rv_peers(), rv_chk(int, int, int, int, int), rv_all();
DROP EXTENSION pg_sift;
