\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.2.2'" to load this file. \quit

-- Keys of the primary key that have more than one row in a table (sift.table_find_stale_versions). Releases 4.0.0 to 4.2.1 could make the old versions of updated rows live again
-- after a VACUUM (fixed in 4.2.2); a primary key is then found on more than one row. The table is read by a sequential scan that the aggregate engine and the primary key index
-- cannot answer, which would hide the copies. A row that was deleted and came back has no copy: this function cannot find it.
CREATE OR REPLACE FUNCTION sift.table_find_stale_versions(tbl regclass, sample integer DEFAULT 10)
	RETURNS TABLE (duplicate_keys bigint, sample_keys text[])
	LANGUAGE plpgsql STABLE
	SET sift.enable_agg = off SET enable_indexscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off
AS $$
DECLARE
	cols text;
BEGIN
	SELECT string_agg(format('%I', a.attname), ', ' ORDER BY k.ord) INTO cols
	FROM pg_index i
	CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY k(attnum, ord)
	JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.attnum
	WHERE i.indrelid = tbl AND i.indisprimary;
	IF cols IS NULL THEN
		RAISE EXCEPTION 'table % has no primary key', tbl USING ERRCODE = 'undefined_object';
	END IF;
	RETURN QUERY EXECUTE format(
		'SELECT count(*), (array_agg(k ORDER BY k))[1:%s] FROM (SELECT ROW(%s)::text AS k FROM %s GROUP BY %s HAVING count(*) > 1) d',
		sample, cols, tbl, cols);
END
$$;

COMMENT ON FUNCTION sift.table_find_stale_versions(regclass, integer) IS 'The number of primary key values of the table that are on more than one row (the old versions of updated rows that a VACUUM of 4.0.0 to 4.2.1 made live again) and up to sample of them; a row that was deleted and came back has no copy and is not found';
