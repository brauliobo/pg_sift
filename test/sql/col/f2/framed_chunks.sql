-- The cold string blocks and split-bit doubles of a sift_table part are cut into frames of 256 values (SIFT_ENC_FRAMED): whole, by ranges of rows and by single rows they give the values that went in,
-- from one granule length to another and with and without nulls, and the part of a sift_col is not framed.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION framed_chunks(bool, int, int) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_f2_framed_chunks';

SELECT kind, n, sum(split_part(r, ' ', 1)::int) AS framed_chunks, sum(split_part(r, ' ', 4)::int) AS differing_values
FROM (VALUES (true, 'strings', 0), (true, 'strings', 1), (false, 'doubles', 1), (false, 'doubles', 5)) k(str, kind, seed), unnest(ARRAY[1, 255, 256, 257, 1000, 4097, 8192]) n,
	LATERAL (SELECT framed_chunks(k.str, n, k.seed) AS r) x GROUP BY kind, n ORDER BY kind, n;

DROP FUNCTION framed_chunks(bool, int, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
