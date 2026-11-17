-- The proof of a vector program of an aggregate argument takes only a postfix program that never pops an empty stack: a binary step needs two values, a negation one.
-- Steps: 0 leaf, 1 constant, 2 add, 3 sub, 4 mul, 5 neg, 6 abs (not run by the vector path).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT steps, sift._test_vnum_program_ok(steps) AS ok
FROM (VALUES
	('{0}'::int4[]), ('{0,5}'), ('{0,1,4}'), ('{0,0,2,5}'), ('{0,1,4,0,3}'), ('{0,0,0,2,3}'),
	('{}'), ('{0,2,0}'), ('{5,0}'), ('{0,3,0,0}'), ('{0,0,2,2,0}'), ('{0,6}'), ('{0,0}'),
	('{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2}'),
	('{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2}')) AS t(steps);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
