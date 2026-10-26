-- Foreign keys on a sift_table, as the referencing table and as the referenced one: INSERT, UPDATE and DELETE of either side, and the actions (NO ACTION,
-- CASCADE, SET NULL), give what the same statements give on heap tables. The integrity trigger asks the slot of the old row whether the current transaction made
-- it, so the slot has to say. The rows are in the write buffers (set u) or in column groups (set v).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

-- the outcome of a statement: the rows it changed and what the tables hold after it, or the error; always rolled back
CREATE FUNCTION fk_try(stmt text, probe text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; seen text;
BEGIN
	EXECUTE stmt;
	GET DIAGNOSTICS n = ROW_COUNT;
	EXECUTE probe INTO seen;
	RAISE EXCEPTION '%', n || ' ' || seen USING ERRCODE = 'XXOK1';
EXCEPTION
	WHEN SQLSTATE 'XXOK1' THEN RETURN 'ok ' || SQLERRM;
	WHEN OTHERS THEN RETURN SQLSTATE;
END $$;

-- four pairs (referenced table, referencing table) by the access method of each side; three referencing tables per pair: NO ACTION, CASCADE, SET NULL
CREATE TABLE fk_pair (i int, parent_am text, child_am text);
INSERT INTO fk_pair VALUES (1, 'heap', 'heap'), (2, 'heap', 'sift_table'), (3, 'sift_table', 'heap'), (4, 'sift_table', 'sift_table');
CREATE TABLE fk_action (a int, clause text, name text);
INSERT INTO fk_action VALUES (1, '', 'no action'), (2, ' ON DELETE CASCADE ON UPDATE CASCADE', 'cascade'), (3, ' ON DELETE SET NULL ON UPDATE SET NULL', 'set null');
DO $$
DECLARE r record; a record; s text;
BEGIN
	FOREACH s IN ARRAY ARRAY['u', 'v'] LOOP
		FOR r IN SELECT * FROM fk_pair ORDER BY i LOOP
			EXECUTE format('CREATE TABLE p_%s%s (id int PRIMARY KEY, w int) USING %s', s, r.i, r.parent_am);
			EXECUTE format('INSERT INTO p_%s%s SELECT i, 0 FROM generate_series(1, 8) i', s, r.i);
			FOR a IN SELECT * FROM fk_action ORDER BY a LOOP
				EXECUTE format('CREATE TABLE c_%s%s%s (id int, pid int REFERENCES p_%s%s(id)%s, v int) USING %s', s, r.i, a.a, s, r.i, a.clause, r.child_am);
				EXECUTE format('INSERT INTO c_%s%s%s SELECT i, 1 + i %% 4, 0 FROM generate_series(1, 12) i', s, r.i, a.a);
			END LOOP;
		END LOOP;
	END LOOP;
END $$;
VACUUM (FREEZE) p_v1, p_v2, p_v3, p_v4, c_v11, c_v12, c_v13, c_v21, c_v22, c_v23, c_v31, c_v32, c_v33, c_v41, c_v42, c_v43;

-- @p and @c are the referenced and the referencing table of a pair
CREATE TABLE fk_stmt (n serial, label text, stmt text);
INSERT INTO fk_stmt (label, stmt) VALUES
	('insert child', 'INSERT INTO @c VALUES (100, 1, 0)'),
	('insert child without parent', 'INSERT INTO @c VALUES (100, 99, 0)'),
	('insert child with NULL', 'INSERT INTO @c VALUES (100, NULL, 0)'),
	('update child value', 'UPDATE @c SET v = v + 1 WHERE id = 1'),
	('update all children', 'UPDATE @c SET v = v + 1'),
	('update child key to another parent', 'UPDATE @c SET pid = 2 WHERE id = 1'),
	('update child key to a missing parent', 'UPDATE @c SET pid = 99 WHERE id = 1'),
	('update child key to NULL', 'UPDATE @c SET pid = NULL WHERE id = 1'),
	('delete child', 'DELETE FROM @c WHERE id = 1'),
	('insert and update child in one transaction', 'INSERT INTO @c VALUES (100, 1, 0); UPDATE @c SET v = 1, pid = 2 WHERE id = 100'),
	('insert and delete child in one transaction', 'INSERT INTO @c VALUES (100, 1, 0); DELETE FROM @c WHERE id = 100'),
	('update parent value', 'UPDATE @p SET w = w + 1 WHERE id = 1'),
	('update parent key, referenced', 'UPDATE @p SET id = 50 WHERE id = 1'),
	('update parent key, not referenced', 'UPDATE @p SET id = 50 WHERE id = 8'),
	('delete parent, referenced', 'DELETE FROM @p WHERE id = 1'),
	('delete parent, not referenced', 'DELETE FROM @p WHERE id = 8'),
	('insert and update parent in one transaction', 'INSERT INTO @p VALUES (9, 0); UPDATE @p SET id = 10 WHERE id = 9'),
	('insert, reference and delete parent in one transaction', 'INSERT INTO @p VALUES (9, 0); INSERT INTO @c VALUES (100, 9, 0); DELETE FROM @p WHERE id = 9');

CREATE FUNCTION fk_run(stmt text, s text, i int, a int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE p text := format('p_%s%s', s, i); c text := format('c_%s%s%s', s, i, a);
BEGIN
	RETURN fk_try(replace(replace(stmt, '@p', p), '@c', c),
				  format('SELECT (SELECT count(*) FROM %1$s) || ''/'' || (SELECT count(*) FROM %2$s) || ''/'' || (SELECT count(*) FROM %2$s WHERE pid IS NULL)', p, c));
END $$;

-- the result on heap tables, and whether each pair with a sift_table gives it
CREATE FUNCTION fk_compare(OUT label text, OUT heap_result text, OUT child_same bool, OUT parent_same bool, OUT both_same bool) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE s record; a record; w text;
BEGIN
	FOR s IN SELECT * FROM fk_stmt ORDER BY n LOOP
		FOR a IN SELECT * FROM fk_action ORDER BY a LOOP
			FOREACH w IN ARRAY ARRAY['u', 'v'] LOOP
				label := s.label || ' [' || a.name || ', ' || CASE w WHEN 'u' THEN 'write buffer' ELSE 'column groups' END || ']';
				heap_result := fk_run(s.stmt, w, 1, a.a);
				child_same := fk_run(s.stmt, w, 2, a.a) = heap_result;
				parent_same := fk_run(s.stmt, w, 3, a.a) = heap_result;
				both_same := fk_run(s.stmt, w, 4, a.a) = heap_result;
				RETURN NEXT;
			END LOOP;
		END LOOP;
	END LOOP;
END $$;
SELECT * FROM fk_compare();

DO $$
DECLARE t text;
BEGIN
	FOR t IN SELECT relname FROM pg_class WHERE relname ~ '^c_[uv][0-9]+$' AND relkind = 'r' LOOP
		EXECUTE format('DROP TABLE %I', t);
	END LOOP;
	FOR t IN SELECT relname FROM pg_class WHERE relname ~ '^p_[uv][0-9]$' AND relkind = 'r' LOOP
		EXECUTE format('DROP TABLE %I', t);
	END LOOP;
END $$;
DROP TABLE fk_stmt, fk_pair, fk_action;
DROP FUNCTION fk_try(text, text), fk_run(text, text, int, int), fk_compare();
DROP EXTENSION pg_sift;
