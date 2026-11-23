-- The point directory built for a part: each bucket names every row that holds the keys hashed to it, ascending; only nearly distinct keys get one: an integer, uuid or text column that is not the time of the part, nor the lead of its key, and carries no exception, or two to four columns together, at most eight and while the section stays within a quarter of the bytes of the granules, none for the part of a table and none for a small one.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION s55_point_dir(int, int) RETURNS int LANGUAGE C AS 'pg_sift', 'sift_test_s55_point_dir';
SELECT n, s55_point_dir(n, 5) AS failed_expectations FROM unnest(ARRAY[16384, 20000, 40000]) n ORDER BY n;
SELECT s55_point_dir(100, 5);
DROP FUNCTION s55_point_dir(int, int);
DROP EXTENSION pg_sift CASCADE;
