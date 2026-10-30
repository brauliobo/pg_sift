-- Holes: a seeded random sequence of inserts, updates, deletes and VACUUMs over small tables leaves no hole behind a VACUUM that nothing
-- else holds. After each VACUUM of a sift_col index and of a sift_table the file ends at the high-water mark, every free block is
-- reusable, the free blocks are a small share of the file, a second VACUUM changes nothing, and both count and sum what a heap table
-- that took the same statements holds. The sequence is the same on every run.
CREATE EXTENSION pg_sift;
CREATE TABLE fz_ref (id int, grp int, v bigint, note text) WITH (autovacuum_enabled = off);
CREATE TABLE fz_col (LIKE fz_ref) WITH (autovacuum_enabled = off);
CREATE INDEX fz_col_idx ON fz_col USING sift_col (grp, v, note) WITH (key_cut = grp);
CREATE TABLE fz_tbl (LIKE fz_ref) USING sift_table WITH (autovacuum_enabled = off);
CREATE TABLE fz_step (n serial, sql text);
CREATE TABLE fz_bad (n int, what text);

-- 80 steps: the kind of each is drawn from a hash of its number (the same on every run and server), an insert is 5000 to 60000 rows
CREATE FUNCTION fz_draw(n int, k int, m int) RETURNS int LANGUAGE sql IMMUTABLE
	AS $$ SELECT ((n::bigint * 2654435761 + k::bigint * 40503) % 4294967296 / 65536 % m)::int $$;
INSERT INTO fz_step (sql)
SELECT CASE fz_draw(n, 1, 10)
		WHEN 0 THEN 'INSERT INTO @T SELECT g + (SELECT coalesce(max(id), 0) FROM @T), g % 40, g % 500, md5(g::text) FROM generate_series(1, ' || (5000 + fz_draw(n, 2, 55000)) || ') g'
		WHEN 1 THEN 'INSERT INTO @T SELECT g + (SELECT coalesce(max(id), 0) FROM @T), g % 40, g % 500, md5(g::text) FROM generate_series(1, ' || (5000 + fz_draw(n, 2, 55000)) || ') g'
		WHEN 2 THEN 'UPDATE @T SET v = v + 1 WHERE id % ' || (2 + fz_draw(n, 3, 8)) || ' = 0'
		WHEN 3 THEN 'UPDATE @T SET v = v + 1 WHERE grp = ' || fz_draw(n, 4, 40)
		WHEN 4 THEN 'DELETE FROM @T WHERE id % ' || (7 + fz_draw(n, 5, 9)) || ' = 0'
		WHEN 5 THEN 'DELETE FROM @T WHERE grp = ' || fz_draw(n, 6, 40)
		WHEN 6 THEN 'DELETE FROM @T WHERE grp < ' || (1 + fz_draw(n, 7, 12))
		ELSE 'VACUUM @T' END
FROM generate_series(1, 80) n;

CREATE FUNCTION fz_space(t text, OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8, OUT reusable_blocks int8) LANGUAGE plpgsql AS $$
BEGIN
	IF t = 'fz_col' THEN
		SELECT i.relation_blocks, i.high_water, i.free_blocks, i.reusable_blocks INTO fz_space.relation_blocks, fz_space.high_water, fz_space.free_blocks, fz_space.reusable_blocks
			FROM sift.col_index_info('fz_col_idx') i;
	ELSE
		SELECT i.relation_blocks, i.high_water, i.free_blocks, i.reusable_blocks INTO fz_space.relation_blocks, fz_space.high_water, fz_space.free_blocks, fz_space.reusable_blocks
			FROM sift.table_space('fz_tbl') i;
	END IF;
END $$;

-- after the VACUUM of step n: the state it must leave (kept in fz_prev for the second VACUUM, which must change nothing) and the rows
CREATE TABLE fz_prev (t text PRIMARY KEY, relation_blocks int8, free_blocks int8);
CREATE FUNCTION fz_check(tn text, n int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE a record; own int8[]; ref int8[];
BEGIN
	a := fz_space(tn);
	IF a.relation_blocks <> a.high_water THEN INSERT INTO fz_bad VALUES (n, tn || ': the file does not end at the high-water mark'); END IF;
	IF a.reusable_blocks <> a.free_blocks THEN INSERT INTO fz_bad VALUES (n, tn || ': free blocks that are not reusable'); END IF;
	IF a.free_blocks * 10 > a.relation_blocks AND a.free_blocks > 16 THEN INSERT INTO fz_bad VALUES (n, tn || ': more than a tenth of the file is free (' || a.free_blocks || ' of ' || a.relation_blocks || ' blocks)'); END IF;
	INSERT INTO fz_prev VALUES (tn, a.relation_blocks, a.free_blocks) ON CONFLICT (t) DO UPDATE SET relation_blocks = excluded.relation_blocks, free_blocks = excluded.free_blocks;
	EXECUTE format('SELECT ARRAY[count(*), coalesce(sum(v), 0)] FROM %I', tn) INTO own;
	SELECT ARRAY[count(*), coalesce(sum(v), 0)] INTO ref FROM fz_ref;
	IF own <> ref THEN INSERT INTO fz_bad VALUES (n, tn || ': ' || own::text || ' rows and sum against ' || ref::text); END IF;
END $$;
CREATE FUNCTION fz_settled(tn text, n int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE a record; p fz_prev;
BEGIN
	a := fz_space(tn);
	SELECT * INTO p FROM fz_prev WHERE fz_prev.t = tn;
	IF a.relation_blocks > p.relation_blocks OR a.free_blocks > p.free_blocks THEN INSERT INTO fz_bad VALUES (n, tn || ': a second VACUUM changes the space'); END IF;
END $$;
SET enable_seqscan = off;

-- every step on the three tables, the checks after each VACUUM of the two sift ones
\set ECHO none
SELECT stmt FROM (
	SELECT n, t, 0 AS part, format('%s;', replace(sql, '@T', t)) AS stmt FROM fz_step, unnest(ARRAY['fz_ref', 'fz_col', 'fz_tbl']) t
	UNION ALL
	SELECT n, t, 1, format('DO $$ BEGIN PERFORM fz_check(%L, %s); END $$', t, n) FROM fz_step, unnest(ARRAY['fz_col', 'fz_tbl']) t WHERE sql LIKE 'VACUUM%'
	UNION ALL
	SELECT n, t, 2, format('VACUUM %s', t) FROM fz_step, unnest(ARRAY['fz_col', 'fz_tbl']) t WHERE sql LIKE 'VACUUM%'
	UNION ALL
	SELECT n, t, 3, format('DO $$ BEGIN PERFORM fz_settled(%L, %s); END $$', t, n) FROM fz_step, unnest(ARRAY['fz_col', 'fz_tbl']) t WHERE sql LIKE 'VACUUM%'
) q ORDER BY n, part, t \gexec
\set ECHO all

SELECT * FROM fz_bad ORDER BY n, what;
SELECT count(*) > 5 AS vacuums_checked FROM fz_step WHERE sql LIKE 'VACUUM%';
SELECT sift.col_verify('fz_col_idx', true);
DROP TABLE fz_ref, fz_col, fz_tbl, fz_step, fz_bad, fz_prev;
DROP FUNCTION fz_check(text, int), fz_settled(text, int), fz_space(text), fz_draw(int, int, int);
DROP EXTENSION pg_sift;
