-- The work that grew with the size of its input (the stage of a transaction, the IN lists, the delta tails of the deletion vectors, the bulk deletes, the column deltas, the part pins and
-- the address indexes) returns what it did before: every query here is run on a sift_table and on a heap table that holds the same rows, and each line says whether the two agree.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ev (id int PRIMARY KEY, bucket int, f float8, s text, v int) USING sift_table;
CREATE TABLE ev_ref (id int PRIMARY KEY, bucket int, f float8, s text, v int);

CREATE FUNCTION agree(pred text) RETURNS boolean LANGUAGE plpgsql AS
$$
DECLARE a text; b text;
BEGIN
	EXECUTE 'SELECT count(*) || ''/'' || coalesce(sum(v), 0) || ''/'' || coalesce(max(id), 0) FROM ev WHERE ' || pred INTO a;
	EXECUTE 'SELECT count(*) || ''/'' || coalesce(sum(v), 0) || ''/'' || coalesce(max(id), 0) FROM ev_ref WHERE ' || pred INTO b;
	RETURN a = b;
END
$$;

CREATE PROCEDURE apply_both(stmt text) LANGUAGE plpgsql AS
$$
BEGIN
	EXECUTE replace(stmt, 'TBL', 'ev');
	EXECUTE replace(stmt, 'TBL', 'ev_ref');
END
$$;

-- one transaction: rows for many partitions, deletes, a savepoint that rolls back, reads of rows it staged
BEGIN;
CALL apply_both($$INSERT INTO TBL SELECT i, i % 40, CASE WHEN i % 97 = 0 THEN 'NaN'::float8 WHEN i % 89 = 0 THEN '-0'::float8 ELSE (i % 500) / 4.0 END, 'k' || (i % 700), i FROM generate_series(1, 30000) i$$);
SELECT agree('id BETWEEN 100 AND 200') AS point_read_of_staged_rows;
SAVEPOINT a;
CALL apply_both($$DELETE FROM TBL WHERE id % 7 = 0$$);
CALL apply_both($$UPDATE TBL SET v = v + 1 WHERE id % 13 = 0$$);
ROLLBACK TO a;
SELECT agree('true') AS after_rollback_to_savepoint;
CALL apply_both($$DELETE FROM TBL WHERE id % 11 = 0$$);
CALL apply_both($$UPDATE TBL SET v = v + 5 WHERE id % 17 = 0$$);
SELECT agree('id % 17 = 0') AS update_then_read;
SELECT agree('id BETWEEN 1 AND 30000') AS whole_range_in_the_transaction;
COMMIT;
SELECT agree('true') AS committed;

-- long IN lists: integers, floats (NaN, -0 and 0), text
SELECT agree('id IN (' || (SELECT string_agg((i * 7)::text, ',') FROM generate_series(1, 2000) i) || ')') AS in_list_of_integers;
SELECT agree('s IN (' || (SELECT string_agg(quote_literal('k' || (i * 3)), ',') FROM generate_series(1, 400) i) || ')') AS in_list_of_text;
SELECT agree('f IN (''NaN'', ''-0'', 0, ' || (SELECT string_agg((i / 4.0)::text, ',') FROM generate_series(1, 60) i) || ')') AS in_list_of_floats;
SELECT agree('f NOT IN (''NaN'', 0, ' || (SELECT string_agg((i / 4.0)::text, ',') FROM generate_series(1, 60) i) || ')') AS not_in_list_of_floats;
SELECT agree('s NOT IN (' || (SELECT string_agg(quote_literal('k' || (i * 3)), ',') FROM generate_series(1, 400) i) || ')') AS not_in_list_of_text;

-- the bulk path of a delete, then a vacuum that folds the deletes
CALL apply_both($$DELETE FROM TBL WHERE id % 5 = 0$$);
SELECT agree('true') AS after_bulk_delete;
VACUUM ev;
SELECT agree('true') AS after_vacuum;
CALL apply_both($$UPDATE TBL SET v = v + 1 WHERE id BETWEEN 1000 AND 1500$$);
SELECT agree('id BETWEEN 900 AND 1600') AS after_update_of_a_range;
CALL apply_both($$DELETE FROM TBL WHERE id BETWEEN 2000 AND 2100$$);
SELECT agree('id BETWEEN 1900 AND 2200') AS after_delete_of_a_range;

-- the same, read through the stage of a second transaction that deletes and reads back
BEGIN;
CALL apply_both($$INSERT INTO TBL SELECT i, i % 40, i / 3.0, 'z' || i, i FROM generate_series(30001, 30400) i$$);
CALL apply_both($$DELETE FROM TBL WHERE id BETWEEN 30001 AND 30100$$);
SELECT agree('id > 29990') AS staged_rows_and_deletes;
ROLLBACK;
SELECT agree('id > 29990') AS after_rollback;

DROP TABLE ev;
DROP TABLE ev_ref;
DROP FUNCTION agree(text);
DROP PROCEDURE apply_both(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
