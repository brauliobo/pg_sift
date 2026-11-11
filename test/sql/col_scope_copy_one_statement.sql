-- A scope copy stays exact when one statement changes the facts and the pairs together (data-modifying CTEs): the triggers of the two tables
-- run one after the other on the state after the whole statement, in either order. A partitioned copy gets the partition of a new key in both.
CREATE EXTENSION pg_sift;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL);
CREATE INDEX ON facts (dim_a_id);
CREATE TABLE pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id));
INSERT INTO pairs VALUES (1, 1), (2, 1), (2, 2), (3, 2);
INSERT INTO facts SELECT i, 1 + i % 4, i % 3, i FROM generate_series(1, 40) i;
SELECT sift.scope_create('copy_flat', 'facts', 'dim_a_id', 'pairs', 'scope_id');
SELECT sift.scope_create('copy_part', 'facts', 'dim_a_id', 'pairs', 'scope_id', partition => true);

CREATE FUNCTION state(label text) RETURNS TABLE (edit text, flat_differs bigint, part_differs bigint, copy_rows bigint, join_rows bigint) LANGUAGE sql AS $$
	SELECT label, (SELECT count(*) FROM sift.scope_verify('copy_flat')), (SELECT count(*) FROM sift.scope_verify('copy_part')),
		   (SELECT count(*) FROM copy_part), (SELECT count(*) FROM facts JOIN pairs USING (dim_a_id)) $$;
CREATE TABLE edits (n serial, label text, stmt text);
INSERT INTO edits (label, stmt) VALUES
	('a pair of a new key and facts of its id', 'WITH p AS (INSERT INTO pairs VALUES (4, 7) RETURNING 1) INSERT INTO facts SELECT 100 + i, 4, 1, i FROM generate_series(1, 5) i'),
	('a pair of a new key and an update of the facts of its id', 'WITH p AS (INSERT INTO pairs VALUES (1, 8) RETURNING 1) UPDATE facts SET amount = amount + 1 WHERE dim_a_id = 1'),
	('a pair moves to a new key and the facts change id', 'WITH p AS (UPDATE pairs SET scope_id = 9 WHERE dim_a_id = 3 AND scope_id = 2 RETURNING 1) UPDATE facts SET dim_a_id = 3 WHERE id % 5 = 0'),
	('the facts of an id go and the id gains a key', 'WITH f AS (DELETE FROM facts WHERE dim_a_id = 2 RETURNING dim_a_id) INSERT INTO pairs SELECT DISTINCT dim_a_id, 10 FROM f'),
	('a key goes and the facts change', 'WITH d AS (DELETE FROM pairs WHERE scope_id = 1 RETURNING 1) UPDATE facts SET kind = 2 WHERE id % 3 = 0');
CREATE FUNCTION run_edits() RETURNS TABLE (edit text, flat_differs bigint, part_differs bigint, copy_rows bigint, join_rows bigint) LANGUAGE plpgsql AS $$
DECLARE e record;
BEGIN
	FOR e IN SELECT * FROM edits ORDER BY n LOOP
		EXECUTE e.stmt;
		RETURN QUERY SELECT * FROM state(e.label);
	END LOOP;
END $$;
-- a new key makes an index of its own and says what it chose each time
SET client_min_messages = warning;
SELECT * FROM run_edits();
RESET client_min_messages;
SELECT count(*) AS partitions FROM pg_inherits WHERE inhparent = 'copy_part'::regclass;

SELECT sift.scope_drop('copy_flat');
SELECT sift.scope_drop('copy_part');
DROP FUNCTION run_edits(), state(text);
DROP TABLE edits, facts, pairs;
DROP EXTENSION pg_sift;
