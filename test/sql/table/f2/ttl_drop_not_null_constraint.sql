-- sift_table: the NOT NULL of the TTL column stays when it is dropped as a constraint (PostgreSQL 18 catalogs it as one), on the table and through the partitioned table above it: a row
-- without a time would be dropped with an expired part. The NOT NULL of another column still drops. Every check prints t.
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE FUNCTION drop_not_nulls(rel regclass, col name) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	c name;
BEGIN
	FOR c IN SELECT n.conname FROM pg_constraint n JOIN pg_attribute a ON a.attrelid = n.conrelid AND a.attnum = n.conkey[1]
			  WHERE n.conrelid = rel AND n.contype = 'n' AND a.attname = col LOOP
		BEGIN
			EXECUTE format('ALTER TABLE %s DROP CONSTRAINT %I', rel, c);
		EXCEPTION WHEN dependent_objects_still_exist THEN
			NULL;
		END;
	END LOOP;
END $$;
CREATE FUNCTION not_null(rel regclass, col name) RETURNS bool LANGUAGE sql AS $$ SELECT attnotnull FROM pg_attribute WHERE attrelid = rel AND attname = col $$;

CREATE TABLE tt (id int NOT NULL, at timestamptz NOT NULL) USING sift_table;
SELECT sift.set_ttl('tt', 'at', '30 days') AS ttl_set;
SELECT drop_not_nulls('tt', 'at');
SELECT not_null('tt', 'at') AS ttl_column_stays_not_null;
SELECT drop_not_nulls('tt', 'id');
SELECT NOT not_null('tt', 'id') AS other_column_drops_not_null;

CREATE TABLE pt (id int, at timestamptz NOT NULL) PARTITION BY RANGE (id);
CREATE TABLE pt_1 PARTITION OF pt FOR VALUES FROM (0) TO (100) USING sift_table;
SELECT sift.set_ttl('pt_1', 'at', '30 days') AS partition_ttl_set;
SELECT drop_not_nulls('pt', 'at');
SELECT not_null('pt_1', 'at') AS partition_ttl_column_stays_not_null;

DROP TABLE tt, pt;
DROP FUNCTION drop_not_nulls(regclass, name), not_null(regclass, name);
DROP EXTENSION pg_sift;
