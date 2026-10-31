-- ORDER BY key, id LIMIT n OFFSET m over the global order of the key (two sort keys, the second the tie-break): the groups of rows that tie on the first key and lie wholly before the
-- OFFSET are counted and not fetched, when their units are exact (no dead row, pages all-visible, the condition decided by the key); the group that holds the first row to keep is
-- fetched whole and sorted by the tie-break key. The rows are the plan's without the extension through ties across the boundary, nulls, deleted and updated rows, a VACUUM, new rows
-- and a merge; the heap fetches say which entries were confirmed.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

CREATE TABLE ot (id int, a int2, b int, t timestamp, pad text DEFAULT repeat('x', 200));
ALTER TABLE ot ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ot (id, a, b, t)
SELECT i, CASE WHEN i % 29 = 0 THEN NULL ELSE (i * 7) % 60 - 30 END, CASE WHEN i % 31 = 0 THEN NULL ELSE (i * 7919) % 500 END,
	   CASE WHEN i % 43 = 0 THEN NULL ELSE TIMESTAMP '2000-01-01' + ((i * 7919) % 900000) * interval '1 second' END
FROM generate_series(1, 12000) i;
CREATE INDEX ot_i ON ot USING sift_col (id sift.col_int4_ops, a sift.col_int2_ops, b sift.col_int4_ops, t sift.col_timestamp_ops)
	WITH (order_columns = 'id, a, b, t', order_array_columns = 'id, a, b, t');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ot;
SET sift.test_topn_heads = off;

-- the rows are the plan's without the extension, in order (id breaks every tie); the answer says 'skipped' when the scan fetched no more than the LIMIT and the largest group of ties
-- (414 rows: the nulls of a), 'fetched' when it fetched more, 'units' when the walk of the units answered, and 'same' for an OFFSET below 1,000, where the fetches say nothing
CREATE FUNCTION ot_chk(col text, dir text, nl text, lim int, off int, other text DEFAULT '') RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text; pl text; used bool := false; fetches int := -1; ka text; kb text;
BEGIN
	q := format('SELECT id, %1$s AS k FROM ot %5$s ORDER BY %1$s %2$s NULLS %3$s, id LIMIT %4$s OFFSET %6$s', col, dir, nl, lim, other, off);
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		used := used OR pl LIKE '%Global Order%';
		IF pl LIKE '%heap fetches%' THEN fetches := substring(pl from '([0-9]+) heap fetches')::int; END IF;
	END LOOP;
	EXECUTE 'SELECT string_agg(id || '':'' || coalesce(k::text, ''N''), '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO ka;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE 'SELECT string_agg(id || '':'' || coalesce(k::text, ''N''), '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO kb;
	RETURN CASE WHEN ka IS DISTINCT FROM kb THEN 'DIFFERS ' || col || ' ' || dir || ' ' || nl || ' ' || lim || ' ' || off || ' ' || other
				WHEN NOT used THEN 'units'
				WHEN off < 1000 THEN 'same' WHEN fetches <= lim + 414 THEN 'skipped' ELSE 'fetched' END;
END $$;

CREATE FUNCTION ot_all(lim int, offs int[], other text DEFAULT '') RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT result, count(*) FROM (SELECT ot_chk(c, d, nl, lim, o, other) AS result FROM unnest(ARRAY['a', 'b', 't']) c, unnest(ARRAY['ASC', 'DESC']) d,
	unnest(ARRAY['FIRST', 'LAST']) nl, unnest(offs) o) r GROUP BY 1 ORDER BY 1 $$;

-- every unit exact: the groups before the OFFSET are not fetched (an offset inside a group of ties, at its edge, past the nulls); near the end of 12,000 rows the boundary group and the tail after it are fetched
SELECT * FROM ot_all(10, ARRAY[1, 7, 100, 199, 200, 201, 3000, 11990]);
SELECT * FROM ot_all(300, ARRAY[100, 3000]);
-- a condition on another column that the array cannot decide alone reads the heap: no group is skipped, the rows are the same
SELECT result, count(*) FROM (
	SELECT ot_chk(c, d, nl, 10, o, 'WHERE ' || w) AS result
	FROM (VALUES ('b', 'b IS NOT NULL'), ('b', 'b > 100 AND a > 0'), ('t', 't BETWEEN ''2000-03-01'' AND ''2000-04-01'''), ('a', 'a <> 3 AND a < 20')) q(c, w),
		 unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['FIRST', 'LAST']) nl, unnest(ARRAY[1, 25, 800]) o) r
GROUP BY 1 ORDER BY 1;

-- rows of the write buffer, deleted rows (the pages are not all-visible until a VACUUM) and updated rows
INSERT INTO ot (id, a, b, t) SELECT 12000 + i, (i * 3) % 40, NULLIF(i % 17, 0), TIMESTAMP '2021-01-01' + i * interval '1 minute' FROM generate_series(1, 1500) i;
DELETE FROM ot WHERE id % 11 = 0;
UPDATE ot SET a = coalesce(a, 0) + 1, b = coalesce(b, 0) + 1, t = t + interval '1 day' WHERE id % 7 = 0;
SELECT * FROM ot_all(10, ARRAY[1, 7, 100, 3000]);
-- a VACUUM flags the dead rows of the units: those units are confirmed again
VACUUM ot;
SELECT * FROM ot_all(10, ARRAY[1, 7, 100, 3000]);
-- addresses the VACUUM freed are taken by new rows: an entry of the old row names a row with another key
INSERT INTO ot (id, a, b, t) SELECT 20000 + i, -i % 50, i % 400, TIMESTAMP '1990-01-01' + i * interval '1 hour' FROM generate_series(1, 900) i;
SELECT * FROM ot_all(10, ARRAY[1, 7, 100, 3000]);
SELECT sift.col_promote('ot_i'::regclass) >= 0 AS promoted;
SELECT * FROM ot_all(10, ARRAY[1, 7, 100, 3000]);
SELECT sift.col_merge('ot_i'::regclass, true) >= 0 AS merged;
SELECT * FROM ot_all(10, ARRAY[1, 7, 100, 3000]);

DROP FUNCTION ot_all(int, int[], text), ot_chk(text, text, text, int, int, text);
DROP TABLE ot;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
