-- A dictionary stored in blocks that the backend has not loaded gives the executor the strings of the codes it reads, by decompressing their blocks
-- (SiftColDict.get), where the statement used to inflate the whole dictionary first. A request over a few groups loads none; a dictionary that the statements
-- ask for in many blocks is loaded whole, and the one a predicate, a top-N or a lookup reads whole is loaded when it is read. The rows are those of the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
SET sift.agg_force = on;
SET sift.async_maintenance = off;

CREATE TABLE lz (id int PRIMARY KEY, grp int, word text, other text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO lz
SELECT i, i % 50,
       CASE WHEN i % 29 = 0 THEN NULL WHEN i % 31 = 0 THEN '' ELSE 'w' || md5((i % 20000)::text) END,
       'o' || md5((i % 7000)::text),
       i % 100
FROM generate_series(1, 120000) i;
CREATE INDEX lz_col ON lz USING sift_col (grp, word, other, amount) WITH (order_array_columns = 'off');
VACUUM (FREEZE, ANALYZE) lz;
SELECT count(*) AS segments FROM sift.col_segments('lz_col') WHERE kind = 'immutable';

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM (TABLE r_on EXCEPT TABLE r_off) x) + (SELECT count(*) FROM (TABLE r_off EXCEPT TABLE r_on) x);
	RETURN NEXT;
END $$;

-- a new backend reads the words of the groups it keeps and parses no dictionary
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT * FROM same('SELECT word, count(*), sum(amount) FROM lz WHERE id < 40 GROUP BY 1');
SELECT * FROM same('SELECT other, word, count(*) FROM lz WHERE id BETWEEN 1000 AND 1060 GROUP BY 1, 2');
SELECT * FROM same('SELECT grp, count(DISTINCT word) FROM lz WHERE id < 100 GROUP BY 1');
SELECT * FROM same('SELECT upper(word), count(*) FROM lz WHERE id < 40 GROUP BY 1');
SELECT loads = 0 AS no_dictionary_parsed FROM sift._test_col_dict_cache(true);

-- every word of the dictionary is asked for: the blocks that cost the parse are loaded whole once, the answers do not change
SELECT * FROM same('SELECT word, count(*) FROM lz GROUP BY 1');
SELECT loads <= 2 AS parsed_whole FROM sift._test_col_dict_cache(true);
SELECT * FROM same('SELECT word, count(*) FROM lz WHERE id < 40 GROUP BY 1');
SELECT loads = 0 AS found_in_the_cache FROM sift._test_col_dict_cache(true);

-- the readers of the whole dictionary
REINDEX INDEX lz_col;
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT * FROM same('SELECT grp, count(*) FROM lz WHERE word LIKE ''w00%'' GROUP BY 1');
SELECT * FROM same('SELECT word, count(*) FROM lz WHERE word >= ''wf'' AND id < 5000 GROUP BY 1');
SELECT * FROM same('SELECT word, other, amount FROM lz WHERE id < 200 ORDER BY word, id LIMIT 10');
SELECT * FROM same('SELECT word, count(*), min(other) FROM lz WHERE id < 3000 GROUP BY ROLLUP (1)');
SELECT * FROM same('SELECT grp, word, count(*) FROM lz WHERE id < 300 GROUP BY GROUPING SETS ((grp), (word), (grp, word))');
SELECT * FROM same('SELECT word, count(*) FROM lz WHERE word IS NOT NULL AND id < 90 GROUP BY 1 HAVING count(*) > 0');

DROP FUNCTION same(text);
DROP TABLE lz;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
