-- sift_table against heap: the same statements on a heap table and a sift_table must leave identical rows. Every phase compares
-- both ways (EXCEPT ALL), through a sequential scan and through the indexes; a mismatch prints the differing rows.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_tam;

CREATE TABLE h (id int PRIMARY KEY, grp int, qty int, price numeric(10,2), tag text, doc jsonb, ts timestamptz);
CREATE TABLE s (LIKE h INCLUDING ALL) USING sift_table;
CREATE INDEX h_grp ON h (grp, qty);
CREATE INDEX s_grp ON s (grp, qty);
SELECT c.relname, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname IN ('h', 's') ORDER BY 1;

-- runs a statement template on both tables; @T is the table name
CREATE FUNCTION twice(tpl text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(tpl, '@T', 'h');
	EXECUTE replace(tpl, '@T', 's');
END $$;

CREATE FUNCTION compare(what text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	a bigint; b bigint; ia text; ib text;
BEGIN
	SELECT count(*) INTO a FROM (SELECT * FROM h EXCEPT ALL SELECT * FROM s) x;
	SELECT count(*) INTO b FROM (SELECT * FROM s EXCEPT ALL SELECT * FROM h) x;
	SET LOCAL enable_seqscan = off;
	SELECT count(*) || ':' || coalesce(sum(qty), 0) INTO ia FROM h WHERE grp BETWEEN 3 AND 6 AND qty > 10;
	SELECT count(*) || ':' || coalesce(sum(qty), 0) INTO ib FROM s WHERE grp BETWEEN 3 AND 6 AND qty > 10;
	RETURN what || ': ' || CASE WHEN a = 0 AND b = 0 AND ia = ib THEN 'same' ELSE format('DIFFERENT h-s=%s s-h=%s idx %s vs %s', a, b, ia, ib) END;
END $$;

SELECT twice($$INSERT INTO @T SELECT g, g % 10, (g * 7) % 100, (g % 50) * 1.25, 'tag' || g % 13, jsonb_build_object('k', g, 'a', jsonb_build_array(g, g + 1)),
	timestamptz '2025-01-01' + g * interval '1 hour' FROM generate_series(1, 2000) g$$);
SELECT compare('bulk insert');

SELECT twice($$UPDATE @T SET qty = qty + 5, tag = 'upd' WHERE id % 7 = 0$$);
SELECT compare('update non-key');
SELECT twice($$UPDATE @T SET id = id + 100000 WHERE id % 97 = 0$$);
SELECT compare('update key');
SELECT twice($$DELETE FROM @T WHERE id % 11 = 0$$);
SELECT compare('delete');
SELECT twice($$UPDATE @T SET tag = repeat('w', 3000), doc = jsonb_build_object('big', repeat('z', 4000)) WHERE id % 50 = 1$$);
SELECT compare('wide rows');
SELECT twice($$UPDATE @T SET tag = NULL, price = NULL, ts = NULL WHERE id % 13 = 2$$);
SELECT compare('nulls');

-- many small statements, including aborted ones and savepoints
DO $$
DECLARE i int;
BEGIN
	FOR i IN 1 .. 60 LOOP
		PERFORM twice(format('INSERT INTO @T VALUES (%s, %s, %s, 1, ''loop'', NULL, now()) ON CONFLICT (id) DO UPDATE SET qty = excluded.qty + 1',
			3000 + i % 40, i % 10, i));
		IF i % 3 = 0 THEN
			PERFORM twice(format('UPDATE @T SET qty = qty + 1 WHERE id = %s', 3000 + (i * 7) % 40));
		END IF;
		IF i % 5 = 0 THEN
			PERFORM twice(format('DELETE FROM @T WHERE id = %s', 3000 + (i * 3) % 40));
		END IF;
		BEGIN
			PERFORM twice(format('UPDATE @T SET qty = -1 WHERE id %% 9 = %s', i % 9));
			IF i % 4 = 0 THEN RAISE EXCEPTION 'undo'; END IF;
		EXCEPTION WHEN raise_exception THEN NULL;
		END;
	END LOOP;
END $$;
SELECT compare('loop with savepoints');

BEGIN;
SELECT twice($$DELETE FROM @T WHERE grp = 4$$);
SELECT twice($$INSERT INTO @T SELECT g + 500000, 4, 1, 1, 'tx', NULL, now() FROM generate_series(1, 50) g$$);
ROLLBACK;
SELECT compare('rollback');

-- joined statements
SELECT twice($$UPDATE @T t SET qty = o.q FROM (SELECT id, qty * 2 AS q FROM @T WHERE grp = 5) o WHERE t.id = o.id$$);
SELECT compare('update from');
SELECT twice($$DELETE FROM @T t USING (SELECT id FROM @T WHERE grp = 6 AND qty < 30) o WHERE t.id = o.id$$);
SELECT compare('delete using');
SELECT twice($$WITH ins AS (INSERT INTO @T SELECT id + 700000, grp, qty, price, 'cte', doc, ts FROM @T WHERE grp = 2 RETURNING id)
	UPDATE @T SET tag = 'cte-parent' WHERE id IN (SELECT id - 700000 FROM ins)$$);
SELECT compare('data-modifying CTE');
SELECT twice($$MERGE INTO @T t USING (SELECT g AS id, g % 10 AS grp FROM generate_series(1990, 2010) g) m ON t.id = m.id
	WHEN MATCHED AND m.id % 2 = 0 THEN DELETE WHEN MATCHED THEN UPDATE SET tag = 'merged' WHEN NOT MATCHED THEN INSERT (id, grp, qty) VALUES (m.id, m.grp, 0)$$);
SELECT compare('merge');

-- COPY in
CREATE TEMP TABLE copy_src (id int, grp int, qty int, price numeric(10,2), tag text, doc jsonb, ts timestamptz);
INSERT INTO copy_src SELECT g + 900000, g % 10, g, g, 'copy', jsonb_build_object('c', g), now() FROM generate_series(1, 1500) g;
SELECT twice($$INSERT INTO @T SELECT * FROM copy_src$$);
SELECT compare('insert select');

-- reads agree: aggregates, windows, sorts, joins, locking reads
SELECT (SELECT array_agg(x ORDER BY x) FROM (SELECT grp, count(*), sum(qty), min(price), max(tag) FROM h GROUP BY grp) x)
	= (SELECT array_agg(x ORDER BY x) FROM (SELECT grp, count(*), sum(qty), min(price), max(tag) FROM s GROUP BY grp) x) AS aggregates;
SELECT (SELECT array_agg(r ORDER BY r) FROM (SELECT id, row_number() OVER (PARTITION BY grp ORDER BY qty, id), sum(qty) OVER (PARTITION BY grp) FROM h) r)
	= (SELECT array_agg(r ORDER BY r) FROM (SELECT id, row_number() OVER (PARTITION BY grp ORDER BY qty, id), sum(qty) OVER (PARTITION BY grp) FROM s) r) AS windows;
SELECT (SELECT array_agg(h.id ORDER BY h.id) FROM h JOIN s USING (id) WHERE h.qty = s.qty) = (SELECT array_agg(id ORDER BY id) FROM h) AS self_join;
SELECT (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM h WHERE grp = 3 ORDER BY qty DESC, id LIMIT 25 FOR UPDATE) x)
	= (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM s WHERE grp = 3 ORDER BY qty DESC, id LIMIT 25 FOR UPDATE) x) AS for_update;
SELECT (SELECT array_agg(doc->>'k' ORDER BY id) FROM h WHERE doc @> '{"a": [5, 6]}') IS NOT DISTINCT FROM (SELECT array_agg(doc->>'k' ORDER BY id) FROM s WHERE doc @> '{"a": [5, 6]}') AS jsonb_filter;

-- maintenance does not change the answers
VACUUM h; VACUUM s;
SELECT compare('vacuum');
VACUUM (FULL, ANALYZE) h; VACUUM (FULL, ANALYZE) s;
SELECT compare('vacuum full');
REINDEX TABLE s;
SELECT compare('reindex');
CLUSTER s USING s_pkey;
SELECT compare('cluster');
ALTER TABLE s SET ACCESS METHOD heap;
SELECT compare('to heap');
ALTER TABLE s SET ACCESS METHOD sift_table;
SELECT compare('back to sift_table');
TRUNCATE h, s;
SELECT compare('truncate');

DROP TABLE h, s;
DROP EXTENSION pg_sift_tam;
DROP EXTENSION pg_sift;
