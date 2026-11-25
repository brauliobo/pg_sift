-- sift.check names the owner of every block below the high-water mark: a block that a live part and a free list both name, and one that nothing names, are problems; and the sections of a part
-- (SEC_POINT, SEC_PREAGG), its deletion vector, its column delta chain and its micro-slab are read, not trusted.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.test_create_format1 = off;
CREATE FUNCTION dec_own(regclass, int) RETURNS void AS '$libdir/pg_sift', 'sift_test_dec_own' LANGUAGE C STRICT;
CREATE FUNCTION dec_damage(regclass, int) RETURNS void AS '$libdir/pg_sift', 'sift_test_dec_damage' LANGUAGE C STRICT;
CREATE FUNCTION dec_section(regclass, int) RETURNS void AS '$libdir/pg_sift', 'sift_test_dec_section' LANGUAGE C STRICT;
CREATE FUNCTION problems(regclass, text) RETURNS text LANGUAGE sql AS $$
	SELECT count(*) || ' ' || coalesce(bool_or(detail LIKE $2)::text, 'none') FROM sift.check($1) $$;

CREATE TABLE ix (id int, v text);
INSERT INTO ix SELECT i, md5(i::text) FROM generate_series(1, 200) i;
CREATE INDEX ix_idx ON ix USING sift_col (id, v);
CREATE TABLE tb (id int, n bigint) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO tb SELECT i, i * 7 FROM generate_series(1, 20000) i;

-- a healthy index and a healthy table have one owner for every block
SELECT problems('ix_idx', '%'), problems('tb', '%');

-- a live part whose extent is also free
SELECT dec_own('ix_idx', 0);
SELECT problems('ix_idx', '%is named twice%');
SELECT dec_own('tb', 0);
SELECT problems('tb', '%is named twice%');

-- an extent that was allocated and that nothing names
CREATE TABLE ix2 (id int, v text);
INSERT INTO ix2 SELECT i, md5(i::text) FROM generate_series(1, 200) i;
CREATE INDEX ix2_idx ON ix2 USING sift_col (id, v);
CREATE TABLE tb2 (id int, n bigint) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO tb2 SELECT i, i * 7 FROM generate_series(1, 20000) i;
SELECT dec_own('ix2_idx', 1);
SELECT problems('ix2_idx', '%named by no part%');
SELECT dec_own('tb2', 1);
SELECT problems('tb2', '%named by no part%');

-- a point directory whose buckets end past its entries, and one that names a granule the part does not have
CREATE TABLE sp0 (id int, v text);
INSERT INTO sp0 SELECT i, md5(i::text) FROM generate_series(1, 200) i;
CREATE INDEX sp0_idx ON sp0 USING sift_col (id, v);
CREATE TABLE sp1 (id int, v text);
INSERT INTO sp1 SELECT i, md5(i::text) FROM generate_series(1, 200) i;
CREATE INDEX sp1_idx ON sp1 USING sift_col (id, v);
SELECT problems('sp0_idx', '%point%');
SELECT dec_section('sp0_idx', 0);
SELECT problems('sp0_idx', '%point%');
SELECT dec_section('sp1_idx', 1);
SELECT problems('sp1_idx', '%point%');

-- pre-aggregates shorter than the first cell of each granule, and ones that count cells the section does not hold
CREATE TABLE sa0 (id int, v text);
INSERT INTO sa0 SELECT i, md5(i::text) FROM generate_series(1, 200) i;
CREATE INDEX sa0_idx ON sa0 USING sift_col (id, v);
CREATE TABLE sa1 (id int, v text);
INSERT INTO sa1 SELECT i, md5(i::text) FROM generate_series(1, 200) i;
CREATE INDEX sa1_idx ON sa1 USING sift_col (id, v);
SELECT dec_section('sa0_idx', 2);
SELECT problems('sa0_idx', '%pre-aggregate%');
SELECT dec_section('sa1_idx', 3);
SELECT problems('sa1_idx', '%pre-aggregate%');

-- a deletion vector with a byte changed
CREATE TABLE dv (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO dv SELECT i, repeat(md5(i::text), 4) FROM generate_series(1, 20000) i;
DELETE FROM dv WHERE id > 1000;
SELECT problems('dv', '%');
SELECT dec_damage('dv', 1);
SELECT problems('dv', '%deletion vector%');

-- a column delta page with a byte changed
CREATE TABLE cd (id int PRIMARY KEY, bal int, note text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO cd SELECT i, 0, 'x' FROM generate_series(1, 2000) i;
UPDATE cd SET bal = 1 WHERE id % 3 = 0;
SELECT problems('cd', '%');
SELECT dec_damage('cd', 0);
SELECT problems('cd', '%');

-- a batch of the micro-slab that claims rows it does not hold, and one cut short, are damage and not an overrun
CREATE TABLE sl0 (id int, n bigint) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO sl0 SELECT i, i * 7 FROM generate_series(1, 50) i;
CREATE TABLE sl1 (id int, n bigint) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO sl1 SELECT i, i * 7 FROM generate_series(1, 50) i;
SELECT dec_damage('sl0', 2);
SELECT count(*) FROM sl0;
SELECT dec_damage('sl1', 3);
SELECT count(*) FROM sl1;

DROP TABLE ix, tb, ix2, tb2, sp0, sp1, sa0, sa1, dv, cd, sl0, sl1;
DROP FUNCTION problems(regclass, text), dec_own(regclass, int), dec_damage(regclass, int), dec_section(regclass, int);
DROP EXTENSION pg_sift;
