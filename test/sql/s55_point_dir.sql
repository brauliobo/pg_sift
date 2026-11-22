-- The point directory built for a part: each bucket names every row that holds the values hashed to it, ascending; only nearly distinct integer columns that are not the time of the part and
-- carry no exception get one, at most three and while the section stays within a quarter of the bytes of the granules; a sorted part or a small one gets none.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION s55_point_dir(int, int) RETURNS int LANGUAGE C AS 'pg_sift', 'sift_test_s55_point_dir';
SELECT n, s55_point_dir(n, 5) AS failed_expectations FROM unnest(ARRAY[16384, 20000, 40000]) n ORDER BY n;
SELECT s55_point_dir(100, 5);
DROP FUNCTION s55_point_dir(int, int);
DROP EXTENSION pg_sift CASCADE;
