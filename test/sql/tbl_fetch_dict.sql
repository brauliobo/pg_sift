-- The strings a sift_table row fetch reads from a segment whose dictionary is stored for a lookup are read in
-- place, one by one: the first SELECT or UPDATE of a backend does not load the dictionary. After many such reads of a segment the dictionary is loaded and
-- cached as before, and the rows read are the same either way: NULL, empty, long (stored inline), non-ASCII, varchar, and after DML.
CREATE EXTENSION pg_sift_test CASCADE;

CREATE TABLE tf (id int PRIMARY KEY, s text, v varchar(40), n int) USING sift_table;
INSERT INTO tf
SELECT i,
	   CASE WHEN i % 97 = 0 THEN NULL
			WHEN i % 89 = 0 THEN ''
			WHEN i % 83 = 0 THEN 'é日本' || i
			ELSE 'name ' || md5(i::text) END,
	   CASE WHEN i % 61 = 0 THEN NULL ELSE 'v' || (i * 7) % 3000 END,
	   i
FROM generate_series(1, 6000) i;
INSERT INTO tf SELECT 6000 + i, repeat('long', 100) || i, 'x', 6000 + i FROM generate_series(1, 20) i;
CREATE TABLE tf_heap AS SELECT * FROM tf;
VACUUM tf;
SELECT kind, min(format) AS format, sum(rows) AS rows FROM sift.table_segments('tf') WHERE kind = 'columns' GROUP BY kind;

-- a new backend's first reads: no dictionary loaded
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT s, v FROM tf WHERE id = 1234;
SELECT s, v FROM tf WHERE id = 97;
SELECT s, v FROM tf WHERE id = 89;
SELECT s, v FROM tf WHERE id = 83;
SELECT length(s), v FROM tf WHERE id = 6005;
UPDATE tf SET n = n + 1 WHERE id = 77 RETURNING id, s, v, n;
UPDATE tf_heap SET n = n + 1 WHERE id = 77;
SELECT loads = 0 AS no_dictionary_loaded FROM sift._test_col_dict_cache(false);

-- many strings of a segment: the dictionary is loaded once, then cached
SELECT sum(length(s)) AS chars FROM tf WHERE id <= 2000;
SELECT loads > 0 AS loaded FROM sift._test_col_dict_cache(false);
SELECT count(*) AS differences FROM (SELECT id, s, v FROM tf EXCEPT SELECT id, s, v FROM tf_heap) d;
SELECT count(*) AS differences FROM (SELECT id, s, v FROM tf_heap EXCEPT SELECT id, s, v FROM tf) d;

-- the same after DML and a rewrite (the rows of the last INSERT have a dictionary of a few strings, in a segment of their own)
UPDATE tf SET s = 'changed ' || id, v = 'w' || id WHERE id % 500 = 0;
UPDATE tf_heap SET s = 'changed ' || id, v = 'w' || id WHERE id % 500 = 0;
DELETE FROM tf WHERE id % 701 = 0;
DELETE FROM tf_heap WHERE id % 701 = 0;
INSERT INTO tf SELECT 7000 + i, 'late ' || i % 50, 'z', i FROM generate_series(1, 300) i;
INSERT INTO tf_heap SELECT 7000 + i, 'late ' || i % 50, 'z', i FROM generate_series(1, 300) i;
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT s, v FROM tf WHERE id = 1234;
SELECT s, v FROM tf WHERE id = 7005;
SELECT loads = 0 AS no_dictionary_loaded_for_the_big_segment FROM sift._test_col_dict_cache(false);
VACUUM tf;
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT s, v FROM tf WHERE id = 1234;
SELECT s, v FROM tf WHERE id = 7005;
SELECT count(*) AS differences FROM (SELECT id, s, v, n FROM tf EXCEPT SELECT id, s, v, n FROM tf_heap) d;
SELECT count(*) AS differences FROM (SELECT id, s, v, n FROM tf_heap EXCEPT SELECT id, s, v, n FROM tf) d;

DROP TABLE tf, tf_heap;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
