-- The tombstone log of a sift_table is read, and sorted by key, by a backend at its first statement: a log of 30,000 entries written in an order
-- that is not the order of the keys (each statement deletes or updates a stripe of the table, the stripes out of order) gives a new session the rows
-- a heap twin gives, for a scan, a lookup of a deleted row and one of an updated row.
CREATE EXTENSION pg_sift;
CREATE TABLE tm (id int, v int) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE th (id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO tm SELECT g, g FROM generate_series(1, 30000) g;
INSERT INTO th SELECT g, g FROM generate_series(1, 30000) g;
VACUUM tm;
DO $$
DECLARE
	t text;
	s int;
BEGIN
	FOREACH t IN ARRAY ARRAY['tm', 'th'] LOOP
		FOR i IN 0..46 LOOP
			s := (i * 17) % 47;
			EXECUTE format('UPDATE %I SET v = v + 1 WHERE id %% 47 = %s AND id %% 5 <> 0', t, s);
			EXECUTE format('DELETE FROM %I WHERE id %% 47 = %s AND id %% 5 = 0', t, s);
		END LOOP;
	END LOOP;
END $$;
\c
SELECT (SELECT count(*) FROM tm) = (SELECT count(*) FROM th) AS same_count,
       (SELECT sum(v) FROM tm) = (SELECT sum(v) FROM th) AS same_sum,
       (SELECT count(*) FROM (SELECT * FROM tm EXCEPT SELECT * FROM th) a) AS only_in_table,
       (SELECT count(*) FROM (SELECT * FROM th EXCEPT SELECT * FROM tm) b) AS only_in_twin;
\c
SELECT id, v FROM tm WHERE id IN (5, 10, 94, 95, 96, 29999, 30000) ORDER BY id;
SELECT id, v FROM th WHERE id IN (5, 10, 94, 95, 96, 29999, 30000) ORDER BY id;
-- a key updated many times in one transaction: the versions the transaction deleted are no rows for its unique check and its scans, the one it updated
-- last is; a rolled back savepoint keeps its updates out, and a key it deleted can be inserted again
CREATE TABLE su (id int PRIMARY KEY, v int) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE sh (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = false);
INSERT INTO su SELECT g, g FROM generate_series(1, 200) g;
INSERT INTO sh SELECT g, g FROM generate_series(1, 200) g;
VACUUM su;
DO $$
DECLARE
	t text;
	dup int;
BEGIN
	FOREACH t IN ARRAY ARRAY['su', 'sh'] LOOP
		FOR i IN 1..6 LOOP
			EXECUTE format('UPDATE %I SET v = v + 1 WHERE id <= 20', t);
		END LOOP;
		BEGIN
			EXECUTE format('UPDATE %I SET v = v + 100 WHERE id <= 20', t);
			RAISE EXCEPTION 'undo';
		EXCEPTION WHEN raise_exception THEN
			NULL;
		END;
		dup := 0;
		BEGIN
			EXECUTE format('INSERT INTO %I VALUES (7, 0)', t);
		EXCEPTION WHEN unique_violation THEN
			dup := 1;
		END;
		RAISE NOTICE '% duplicate refused: %, rows of 7: %, sum of 1..20: %', t, dup, (SELECT count(*) FROM su WHERE t = 'su' AND id = 7) + (SELECT count(*) FROM sh WHERE t = 'sh' AND id = 7), CASE t WHEN 'su' THEN (SELECT sum(v) FROM su WHERE id <= 20) ELSE (SELECT sum(v) FROM sh WHERE id <= 20) END;
		EXECUTE format('DELETE FROM %I WHERE id = 9', t);
		EXECUTE format('INSERT INTO %I VALUES (9, -1)', t);
	END LOOP;
END $$;
SELECT (SELECT count(*) FROM (SELECT * FROM su EXCEPT SELECT * FROM sh) a) AS only_in_table,
       (SELECT count(*) FROM (SELECT * FROM sh EXCEPT SELECT * FROM su) b) AS only_in_twin,
       (SELECT v FROM su WHERE id = 9) AS reinserted;
DROP TABLE su, sh;
DROP TABLE th, tm;
DROP EXTENSION pg_sift;
