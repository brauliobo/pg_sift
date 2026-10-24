-- The limits a user can reach say what to do: SQLSTATE, message, detail and hint of each, with the test-only setting sift.test_limits lowering
-- the limit so that a few rows reach it (src/core/limits.h). scripts/check-errors.sh proves that no 54000, 0A000 or 55000 lacks a hint or a detail.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	st text; m text; d text; h text;
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, m = MESSAGE_TEXT, d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT;
	RETURN concat_ws(E'\n  ', st || ' ' || m, 'detail: ' || nullif(d, ''), 'hint: ' || nullif(h, ''));
END
$$;

CREATE TABLE one (id int, v text);
INSERT INTO one VALUES (1, repeat('x', 200));
CREATE TABLE many (id int, v text);
INSERT INTO many SELECT i, repeat(chr(96 + i), 60) FROM generate_series(1, 5) i;

-- a string over the limit, and the strings of one unit over it
SET sift.test_limits = 'string_bytes=100';
SELECT expect('CREATE INDEX ON one USING sift_col (id, v)');
SELECT expect('CREATE INDEX ON many USING sift_col (id, v)');
RESET sift.test_limits;

-- an index that is not on a heap table
CREATE TABLE tbl (id int, v text) USING sift_table;
SELECT expect('CREATE INDEX ON tbl USING sift_col (id)');

-- more leaves than an index holds: 128 declared paths
CREATE TABLE docs (id int, payload jsonb);
SELECT expect(format('CREATE INDEX ON docs USING sift_col (id, payload) WITH (paths = %L)',
					 (SELECT string_agg(format('payload.k%s int4', i), ', ') FROM generate_series(1, 128) i)));

-- a jsonb path spec over its size
SELECT expect(format('CREATE INDEX ON many USING sift_col (id) WITH (paths = %L)', repeat('a int4,', 20000) || 'b int4'));

-- a row of a sift_table over the write buffer
SET sift.table_write_buffer_limit = 2048;
SET sift.table_wide_rows = off;
SELECT expect($$INSERT INTO tbl VALUES (1, repeat('z', 3000000))$$);
RESET sift.table_wide_rows;
RESET sift.table_write_buffer_limit;

-- sift.explain_why takes one statement; a score outside a sift scan says where it exists
SELECT expect($$SELECT * FROM sift.explain_why('select 1; select 2')$$);
SELECT expect('SELECT sift.score(ctid) FROM many');

DROP TABLE one, many, tbl, docs;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
