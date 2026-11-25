-- An index of the default class gives a statement that aggregates no index or bitmap path, so that SiftAgg and the normal plan are chosen as in 3.5 for it. SiftAgg reads
-- the index alone: a statement that reads a column the index does not hold (avg(score) over a window of dates) is not its to answer, and the index keeps its paths for the
-- rows of the window; an index that holds every column the statement reads stays SiftAgg's.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

CREATE TABLE pw AS SELECT g id, '2022-01-01'::date + (g * 7919 % 1826) d, (g % 5) + 1 score, repeat('x', 100) pad FROM generate_series(1, 60000) g;
VACUUM (ANALYZE) pw;
CREATE INDEX pw_d ON pw USING sift_col (d);

CREATE FUNCTION pw_plan(q text) RETURNS text LANGUAGE plpgsql SET max_parallel_workers_per_gather = 0 SET enable_seqscan = off AS $$
DECLARE r text = '';
	l text;
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) %s', replace(q, '%win', 'd >= ''2024-03-10'' AND d < ''2024-04-10''')) LOOP r = r || l || E'\n'; END LOOP;
	RETURN CASE WHEN r ~ 'SiftAgg' THEN 'SiftAgg' WHEN r ~ 'Bitmap Heap Scan' THEN 'bitmap scan' WHEN r ~ 'Index Scan' THEN 'index scan' ELSE 'other' END;
END $$;

SELECT pw_plan('SELECT count(*) FROM pw WHERE %win') AS count_of_the_dates,
       pw_plan('SELECT avg(score) FROM pw WHERE %win') AS avg_of_another_column,
       pw_plan('SELECT count(*) FROM pw WHERE %win AND score = 3') AS count_with_another_column,
       pw_plan('SELECT score, count(*) FROM pw WHERE %win GROUP BY score') AS group_by_another_column,
       pw_plan('SELECT d, count(*) FROM pw WHERE %win GROUP BY d') AS group_by_the_dates;

CREATE TABLE pw_ref AS SELECT * FROM pw;
SELECT (SELECT avg(score) FROM pw WHERE d >= '2024-03-10' AND d < '2024-04-10') = (SELECT avg(score) FROM pw_ref WHERE d >= '2024-03-10' AND d < '2024-04-10') AS same_avg,
       (SELECT count(*) FROM pw WHERE d >= '2024-03-10' AND d < '2024-04-10' AND score = 3) = (SELECT count(*) FROM pw_ref WHERE d >= '2024-03-10' AND d < '2024-04-10' AND score = 3) AS same_count;

CREATE INDEX pw_dr ON pw USING sift_col (d, score);
DROP INDEX pw_d;
SELECT pw_plan('SELECT avg(score) FROM pw WHERE %win') AS avg_held_by_the_index,
       pw_plan('SELECT avg(score) FROM pw WHERE %win AND id > 10') AS avg_with_a_column_it_lacks;
DROP TABLE pw, pw_ref;
DROP FUNCTION pw_plan(text);
DROP EXTENSION pg_sift;
