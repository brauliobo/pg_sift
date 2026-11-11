-- A scope copy is exact only for writers that see every committed row when their statement starts: a REPEATABLE READ or SERIALIZABLE writer is refused (the
-- statement fails and changes nothing), and the same statement succeeds in READ COMMITTED and leaves the copy exact. Every change of the facts and of the pairs, TRUNCATE too.
CREATE EXTENSION pg_sift;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL);
CREATE INDEX ON facts (dim_a_id);
CREATE TABLE scope_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id));
INSERT INTO scope_pairs VALUES (1, 1), (2, 1), (3, 1), (3, 2), (4, 2);
INSERT INTO facts SELECT i, 1 + i % 5, i % 3, i FROM generate_series(1, 20) i;
SET client_min_messages = warning;
SELECT count(*) > 0 AS built FROM (SELECT sift.scope_create('facts_by_tenant', 'facts', 'dim_a_id', 'scope_pairs', 'tenant_id')) q;
CREATE TABLE edits (n serial, label text, stmt text);
INSERT INTO edits (label, stmt) VALUES
	('fact insert',  'INSERT INTO facts VALUES (100, 2, 1, 5)'),
	('fact update',  'UPDATE facts SET amount = amount + 1, dim_a_id = 3 WHERE id = 1'),
	('fact delete',  'DELETE FROM facts WHERE id = 2'),
	('pair insert',  'INSERT INTO scope_pairs VALUES (5, 3), (2, 2)'),
	('pair update',  'UPDATE scope_pairs SET tenant_id = 4 WHERE dim_a_id = 1 AND tenant_id = 1'),
	('pair delete',  'DELETE FROM scope_pairs WHERE dim_a_id = 3'),
	('pair truncate', 'TRUNCATE scope_pairs'),
	('fact truncate', 'TRUNCATE facts');

-- the edits of one transaction at the isolation level the session starts it with; changed: the facts, the pairs or the copy differ after the edit
CREATE FUNCTION try_edits() RETURNS TABLE (level text, edit text, code text, message text, detail text, hint text, changed bool, copy_differs bigint) LANGUAGE plpgsql AS $$
DECLARE
	e      record;
	before text;
	after  text;
	state  text := 'SELECT md5(concat((SELECT string_agg(f::text, '','' ORDER BY id) FROM facts f), (SELECT string_agg(p::text, '','' ORDER BY 1, 2) FROM scope_pairs p),
		(SELECT string_agg(c::text, '','' ORDER BY 1, 2) FROM facts_by_tenant c)))';
BEGIN
	FOR e IN SELECT * FROM edits ORDER BY n LOOP
		EXECUTE state INTO before;
		level := current_setting('transaction_isolation'); edit := e.label; code := NULL; message := NULL; detail := NULL; hint := NULL;
		BEGIN
			EXECUTE e.stmt;
		EXCEPTION WHEN OTHERS THEN
			GET STACKED DIAGNOSTICS code = RETURNED_SQLSTATE, message = MESSAGE_TEXT, detail = PG_EXCEPTION_DETAIL, hint = PG_EXCEPTION_HINT;
		END;
		EXECUTE state INTO after; changed := after <> before;
		SELECT count(*) INTO copy_differs FROM sift.scope_verify('facts_by_tenant');
		RETURN NEXT;
	END LOOP;
END $$;

SET default_transaction_isolation = 'repeatable read';
SELECT * FROM try_edits();
SET default_transaction_isolation = 'serializable';
SELECT * FROM try_edits();
RESET default_transaction_isolation;
SELECT * FROM try_edits();
SELECT count(*) AS copy_rows, (SELECT count(*) FROM sift.scope_verify('facts_by_tenant')) AS differs FROM facts_by_tenant;

SELECT sift.scope_drop('facts_by_tenant');
DROP FUNCTION try_edits();
DROP TABLE edits, facts, scope_pairs;
DROP EXTENSION pg_sift;
