-- The head of a unit (its row directory and chunk table) is read in 2 kB and the rest of it when the directory ends beyond: a unit of dense pages fits, a unit whose pages
-- hold the rows of every other offset has a bitmap for each page and does not (256 pages of 140 rows: 9 kB). A key-cut unit has its chunk table first and reads its directory when a
-- scan needs the pages or the TIDs. The answers are those of the normal plan in all of them, before and after the changes of a unit.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_preagg = off;
SET sift.agg_force = on;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM (TABLE r_on EXCEPT TABLE r_off) x) + (SELECT count(*) FROM (TABLE r_off EXCEPT TABLE r_on) x);
	RESET sift.enable_agg;
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- dense pages: the head fits the first read
CREATE TABLE uh_dense (id int, grp int, amount int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO uh_dense SELECT g, g % 7, (g * 31) % 1000, 'p' FROM generate_series(1, 60000) g;
CREATE INDEX uh_dense_i ON uh_dense USING sift_col (id, grp, amount);
-- sparse pages: every other row deleted and vacuumed, so each page keeps its offsets up to the highest and a bitmap of them
CREATE TABLE uh_sparse (id int, grp int, amount int) WITH (autovacuum_enabled = off, fillfactor = 100);
INSERT INTO uh_sparse SELECT g, g % 7, (g * 31) % 1000 FROM generate_series(1, 240000) g;
DELETE FROM uh_sparse WHERE id % 2 = 0;
VACUUM uh_sparse;
CREATE INDEX uh_sparse_i ON uh_sparse USING sift_col (id, grp, amount);
-- a key-cut index
CREATE TABLE uh_cut (id int, tenant int, amount int) WITH (autovacuum_enabled = off);
INSERT INTO uh_cut SELECT g, g % 40, (g * 31) % 1000 FROM generate_series(1, 120000) g;
CREATE INDEX uh_cut_i ON uh_cut USING sift_col (id, tenant, amount) WITH (key_cut = 'tenant');
SELECT sift.col_verify('uh_dense_i', true), sift.col_verify('uh_sparse_i', true), sift.col_verify('uh_cut_i', true);

CREATE TABLE q (name text, q text);
INSERT INTO q VALUES
	('dense all units', 'SELECT grp, count(*), sum(amount) FROM uh_dense GROUP BY 1'),
	('dense range', 'SELECT grp, count(*), sum(amount) FROM uh_dense WHERE id BETWEEN 5000 AND 9000 GROUP BY 1'),
	('sparse all units', 'SELECT grp, count(*), sum(amount) FROM uh_sparse GROUP BY 1'),
	('sparse range', 'SELECT grp, count(*), sum(amount) FROM uh_sparse WHERE id BETWEEN 5001 AND 90001 GROUP BY 1'),
	('sparse filter on a measure', 'SELECT grp, count(*), sum(amount) FROM uh_sparse WHERE amount < 40 GROUP BY 1'),
	('sparse ids', 'SELECT count(*), sum(id) FROM uh_sparse WHERE id % 1000 = 1'),
	('key cut tenant', 'SELECT tenant, count(*), sum(amount) FROM uh_cut GROUP BY 1'),
	('key cut one tenant', 'SELECT count(*), sum(amount) FROM uh_cut WHERE tenant = 7');
SELECT name, (same(q)).* FROM q ORDER BY name;

-- a key-cut unit has its chunk table before its directory (column format 34): the scan reads the head only while the pages of the unit are all visible, and the directory when a page is not
SELECT DISTINCT format FROM sift.col_segments('uh_cut_i') WHERE kind = 'immutable';
VACUUM uh_cut;
SELECT name, (same(q)).* FROM q WHERE name LIKE 'key cut%' ORDER BY name;
UPDATE uh_cut SET amount = amount + 1 WHERE id % 97 = 0;
SELECT name, (same(q)).* FROM q WHERE name LIKE 'key cut%' ORDER BY name;
SELECT sift.col_verify('uh_cut_i', true);

-- a unit that changes: new rows in the write buffer, deleted rows, a rewrite of the segments
INSERT INTO uh_sparse SELECT g, g % 7, (g * 31) % 1000 FROM generate_series(240001, 250000) g;
DELETE FROM uh_sparse WHERE id % 3 = 0;
SELECT name, (same(q)).* FROM q WHERE name LIKE 'sparse%' ORDER BY name;
VACUUM uh_sparse;
SELECT sift.col_merge('uh_sparse_i'::regclass, true) >= 0 AS merged;
SELECT name, (same(q)).* FROM q WHERE name LIKE 'sparse%' ORDER BY name;
SELECT sift.col_verify('uh_sparse_i', true);

DROP FUNCTION same(text);
DROP TABLE q, uh_dense, uh_sparse, uh_cut;
DROP EXTENSION pg_sift;
