-- The values the sparse decoders give for a few rows of a granule are those of the whole-column decoders, for integers, doubles and strings of every pattern (a constant, ascending, a few values,
-- random) with and without nulls, in granules of every length class.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION s55_granule_point(int, int) RETURNS int LANGUAGE C AS 'pg_sift', 'sift_test_s55_granule_point';
SELECT n, sum(s55_granule_point(n, seed)) AS differing_values FROM unnest(ARRAY[1, 7, 129, 1000, 4097, 8192]) n, generate_series(1, 3) seed GROUP BY n ORDER BY n;
DROP FUNCTION s55_granule_point(int, int);
DROP EXTENSION pg_sift CASCADE;
