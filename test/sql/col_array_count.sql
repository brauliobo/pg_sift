-- count(*) of one element test that the point directory answers exactly: a segment whose units are all clean and all-visible is counted from the list
-- ("0 rows scanned"); every unit that is not (a dead row, a page not all-visible, an element too common to be listed, rows of the write buffer)
-- is read, and the count is the heap's after every kind of write.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;
SET sift.async_maintenance = off;

CREATE FUNCTION gen(a int, b int) RETURNS TABLE(id int, tags text[], ids int[]) LANGUAGE sql AS $$
	SELECT i, CASE WHEN i % 13 = 0 THEN NULL WHEN i % 17 = 0 THEN '{}' WHEN i % 19 = 0 THEN ARRAY['t1', NULL, 'u1', 't1']
	               ELSE ARRAY['t' || i % 500, 'u' || i % 3, 'w' || i % 1000] END,
	       CASE WHEN i % 11 = 0 THEN NULL WHEN i % 23 = 0 THEN '{}' ELSE ARRAY[i % 400, i % 5 + 1000, i % 7 + 2000] END
	FROM generate_series(a, b) i $$;
CREATE TABLE ac (id int, tags text[], ids int[]) WITH (autovacuum_enabled = false);
INSERT INTO ac SELECT * FROM gen(1, 30000);
CREATE INDEX ac_col ON ac USING sift_col (id, tags, ids) WITH (paths = 'tags[] text, ids[] int4');
VACUUM (FREEZE, ANALYZE) ac;

-- the count with the pushdown (and how many units it read) against the heap's
CREATE FUNCTION cnt(w text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; units text := 'none'; scanned text := ''; a bigint; b bigint;
BEGIN
	SET sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM ac WHERE ' || w LOOP
		IF l ~ '^ *Units:' THEN units := trim(l); END IF;
		IF l ~ '^ *Rows:' THEN scanned := ', ' || (regexp_match(l, 'Rows: (\d+) scanned'))[1] || ' rows scanned'; END IF;
	END LOOP;
	EXECUTE 'SELECT count(*) FROM ac WHERE ' || w INTO a;
	SET sift.enable_agg = off;
	EXECUTE 'SELECT count(*) FROM ac WHERE ' || w INTO b;
	RESET sift.enable_agg;
	RETURN (CASE WHEN a = b THEN 'same ' ELSE 'DIFF ' END) || b || ' ' || regexp_replace(units, ' from point directory.*', ' from point directory') || scanned;
END $$;

CREATE FUNCTION shapes(tag text) RETURNS TABLE(shape text, result text) LANGUAGE sql AS $$
	SELECT tag || ' ' || s.name, cnt(s.w) FROM (VALUES
		('text', $w$tags @> ARRAY['t7']$w$),
		('text any', $w$'t8' = ANY (tags)$w$),
		('repeated', $w$tags @> ARRAY['t1']$w$),
		('int', $w$ids @> ARRAY[7]$w$),
		('absent', $w$tags @> ARRAY['zz']$w$),
		('too common', $w$tags @> ARRAY['u1']$w$),
		('two', $w$tags @> ARRAY['t7', 'w7']$w$),
		('overlap', $w$tags && ARRAY['t7', 't8']$w$),
		('not', $w$NOT (tags @> ARRAY['t7'])$w$),
		('with filter', $w$tags @> ARRAY['t7'] AND id > 100$w$)) s(name, w) $$;

SELECT * FROM shapes('built');
-- rows of the write buffer: the segment is counted from the list, the buffer is read
INSERT INTO ac SELECT * FROM gen(30001, 31000);
SELECT * FROM shapes('insert');
VACUUM ac;
SELECT * FROM shapes('vacuum');
-- a deleted row is dead in the index until it is vacuumed and rewritten, and its page is not all-visible
DELETE FROM ac WHERE id % 1000 = 7;
SELECT * FROM shapes('delete');
VACUUM ac;
SELECT * FROM shapes('delete vacuum');
-- an updated row: the old version is dead, the new one is a row of the buffer
UPDATE ac SET tags = ARRAY['t7', 'x1'] WHERE id % 701 = 0;
SELECT * FROM shapes('update');
VACUUM (FREEZE) ac;
SELECT * FROM shapes('update vacuum');
SELECT sift.col_merge('ac_col', true) > 0 AS merged;
SELECT * FROM shapes('merge');
REINDEX INDEX ac_col;
VACUUM (FREEZE) ac;
SELECT * FROM shapes('reindex');
-- a page that is no longer all-visible while the index is clean: a row of the segment updated in a way that changes no element
UPDATE ac SET id = id WHERE id = 1000;
SELECT * FROM shapes('page');

DROP TABLE ac;
DROP FUNCTION gen(int, int), cnt(text), shapes(text);
DROP EXTENSION pg_sift;
