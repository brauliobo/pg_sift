-- VACUUM of a sift_table holds its dead rows a batch at a time, within maintenance_work_mem (6 bytes a dead row, 16 a run of aborted rows, half of it in all, as the
-- lists double): when a batch is full the liveness of its group rows is cleared, every index goes through the TIDs of the batch, and the next batch starts, as the
-- index passes of a lazy VACUUM whose dead TID list filled the memory. Two identical tables, one vacuumed at the default budget (one batch) and one at the smallest
-- (several), end in the same state: the rows, the entries of the indexes (the dead ones gone, the roots of the chains that go on kept), the segments.
-- The dead rows come from every source: the log of the column groups, a write buffer, HOT updates and a load that was rolled back (rows of a group whose writer aborted).
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pageinspect;
SET client_min_messages = warning;
SET sift.table_write_buffer_limit = 2048;

-- a, b, c: the same rows, vacuumed with the budget of 64 MB (one batch), of 64 kB and of 96 bytes (test); d, e: only the loads, with a batch of one run of 16 bytes and the budget of 64 MB
-- every table: the indexes and the loads (all = the dead rows of the log and of a write buffer as well)
CREATE TEMP TABLE steps (n serial, mode text, sql text);
INSERT INTO steps (mode, sql) VALUES
	('load', 'CREATE TABLE %1$I (id int, grp int, v text, pad text) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false)'),
	('load', 'CREATE INDEX %1$s_id ON %1$I (id) WITH (deduplicate_items = off)'),
	('load', 'CREATE INDEX %1$s_grp ON %1$I (grp) WITH (deduplicate_items = off)'),
	('load', 'CREATE INDEX %1$s_v ON %1$I USING sift (v)'),
	('all', 'INSERT INTO %1$I SELECT g, g %% 7, ''row '' || g, ''p'' FROM generate_series(1, 30000) g'),
	('all', 'VACUUM %1$I'),
	-- the log of the groups: a third deleted, a seventh updated in place (HOT: the chains go on), a tenth moved to another group value
	('all', 'DELETE FROM %1$I WHERE id %% 3 = 0'),
	('all', 'UPDATE %1$I SET pad = ''q'' WHERE id %% 7 = 1'),
	('all', 'UPDATE %1$I SET grp = 99 WHERE id %% 10 = 2'),
	-- the rows of a write buffer, half of them dead
	('all', 'INSERT INTO %1$I SELECT g, g %% 7, ''row '' || g, ''p'' FROM generate_series(30001, 36000) g'),
	('all', 'DELETE FROM %1$I WHERE id > 30000 AND id %% 2 = 0'),
	-- loads into groups, three of them rolled back in the middle of the transaction (groups of rows whose writer aborted: a run of keys each)
	('load', 'BEGIN'),
	('load', 'COPY %1$I FROM PROGRAM $q$seq 40001 41000 | awk ''{ printf "%%d\t%%d\trow %%d\tp\n", $1, $1 %% 7, $1 }''$q$'),
	('load', 'SAVEPOINT s1'),
	('load', 'COPY %1$I FROM PROGRAM $q$seq 42001 43000 | awk ''{ printf "%%d\t%%d\trow %%d\tp\n", $1, $1 %% 7, $1 }''$q$'),
	('load', 'ROLLBACK TO s1'),
	('load', 'COPY %1$I FROM PROGRAM $q$seq 44001 45000 | awk ''{ printf "%%d\t%%d\trow %%d\tp\n", $1, $1 %% 7, $1 }''$q$'),
	('load', 'SAVEPOINT s2'),
	('load', 'COPY %1$I FROM PROGRAM $q$seq 46001 47000 | awk ''{ printf "%%d\t%%d\trow %%d\tp\n", $1, $1 %% 7, $1 }''$q$'),
	('load', 'ROLLBACK TO s2'),
	('load', 'SAVEPOINT s3'),
	('load', 'COPY %1$I FROM PROGRAM $q$seq 48001 49000 | awk ''{ printf "%%d\t%%d\trow %%d\tp\n", $1, $1 %% 7, $1 }''$q$'),
	('load', 'ROLLBACK TO s3'),
	('load', 'COMMIT');
SELECT format(sql, t) FROM unnest(ARRAY['a', 'b', 'c']) t, steps WHERE mode IN ('all', 'load') ORDER BY t, n \gexec
SELECT format(sql, t) FROM unnest(ARRAY['d', 'e']) t, steps WHERE mode = 'load' ORDER BY t, n \gexec

-- the entries of a b-tree: the TIDs its leaf pages hold
CREATE FUNCTION entries(idx regclass) RETURNS bigint LANGUAGE sql AS
$$ SELECT coalesce(sum(s.live_items + s.dead_items - (s.btpo_next <> 0)::int), 0)
   FROM generate_series(1, (pg_relation_size(idx) / 8192)::int - 1) p, LATERAL bt_page_stats(idx::text, p) s WHERE s.type = 'l' $$;
CREATE FUNCTION state(tbl text) RETURNS TABLE (what text, val text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY EXECUTE format('SELECT ''rows'', count(*) || '' '' || sum(id) || '' '' || md5(string_agg(id || grp::text || v || pad, '','' ORDER BY id)) FROM %I', tbl);
	RETURN QUERY EXECUTE format('SELECT ''id entries'', entries(%L)::text', tbl || '_id');
	RETURN QUERY EXECUTE format('SELECT ''grp entries'', entries(%L)::text', tbl || '_grp');
	RETURN QUERY EXECUTE format('SELECT ''bm25 docs'', string_agg(kind || '' '' || ndocs || '' '' || coalesce(dead_docs, 0), '','' ORDER BY gen) FROM sift.segments(%L)', tbl || '_v');
	RETURN QUERY EXECUTE format('SELECT ''segments'', string_agg(kind || '' '' || rows || '' '' || dead_rows, '','' ORDER BY gen) FROM sift.table_segments(%L)', tbl);
END $$;

SELECT what, a.val, a.val = b.val AS b_same, a.val = c.val AS c_same FROM state('a') a JOIN state('b') b USING (what) JOIN state('c') c USING (what) ORDER BY what;
SELECT what, e.val, d.val = e.val AS d_same FROM state('d') d JOIN state('e') e USING (what) ORDER BY what;
SET maintenance_work_mem = '64MB';
VACUUM (VERBOSE) a;
VACUUM (VERBOSE) e;
SET maintenance_work_mem = '64kB';
VACUUM (VERBOSE) b;
RESET maintenance_work_mem;
SET sift.test_vacuum_batch_bytes = 96;
VACUUM (VERBOSE) c;
-- one run of aborted rows (16 bytes) fills a batch
SET sift.test_vacuum_batch_bytes = 16;
VACUUM (VERBOSE) d;
RESET sift.test_vacuum_batch_bytes;
SELECT what, a.val, a.val = b.val AS b_same, a.val = c.val AS c_same FROM state('a') a JOIN state('b') b USING (what) JOIN state('c') c USING (what) ORDER BY what;
SELECT what, e.val, d.val = e.val AS d_same FROM state('d') d JOIN state('e') e USING (what) ORDER BY what;
DROP TABLE a, b, c, d, e;
DROP FUNCTION state(text), entries(regclass);
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
