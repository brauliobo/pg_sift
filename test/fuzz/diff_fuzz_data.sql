-- Data of the differential fuzz (test/fuzz/diff_fuzz.py): small tables with NULLs and duplicates, every row a function of its id and the seed.
-- @SEED@ @ROWS@ @PARTS@ are replaced by the driver.
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE SCHEMA fz;
CREATE FUNCTION fz.h(i bigint, salt int) RETURNS int IMMUTABLE PARALLEL SAFE LANGUAGE sql AS
$$ SELECT ((((i + salt::bigint * 1000003 + @SEED@::bigint * 7919) * 2654435761) % 4294967296) / 65536)::int $$;

-- an enum whose order is neither the order of its OIDs nor the alphabetical one: min < mid < mid2 < high < low
CREATE TYPE fz.lvl AS ENUM ('mid', 'high', 'low');
ALTER TYPE fz.lvl ADD VALUE 'min' BEFORE 'mid';
ALTER TYPE fz.lvl ADD VALUE 'mid2' AFTER 'mid';

CREATE TABLE items (id int PRIMARY KEY, kind text, enabled bool, grp int) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT i, 'k' || fz.h(i, 90) % 5, fz.h(i, 91) % 7 <> 3, CASE WHEN fz.h(i, 92) % 10 = 0 THEN NULL ELSE fz.h(i, 93) % 12 END
FROM generate_series(0, 599) i;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, grp int, a int, b int, ts date, amount int, big int8, val float8, num numeric(12,2),
                     flag bool, doc jsonb, metrics jsonb, ref uuid, blob bytea, title text, wnum numeric(30,4), inum numeric, lv fz.lvl, ivl interval) WITH (autovacuum_enabled = false, fillfactor = 75);
CREATE FUNCTION fz.gen_events(lo bigint, hi bigint, salt int) RETURNS SETOF events LANGUAGE sql AS $$
	SELECT i::int,
	       CASE WHEN fz.h(i, 1 + salt) % 100 < 3 THEN NULL ELSE fz.h(i, 2 + salt) % 600 END,
	       CASE WHEN fz.h(i, 3 + salt) % 100 < 5 THEN NULL ELSE 'k' || fz.h(i, 4 + salt) % 5 END,
	       CASE WHEN fz.h(i, 5 + salt) % 100 < 4 THEN NULL ELSE fz.h(i, 6 + salt) % 12 END,
	       CASE WHEN fz.h(i, 7 + salt) % 100 < 3 THEN NULL ELSE fz.h(i, 8 + salt) % 210 END,
	       CASE WHEN fz.h(i, 9 + salt) % 100 < 3 THEN NULL ELSE fz.h(i, 10 + salt) % 105 END,
	       CASE WHEN fz.h(i, 11 + salt) % 100 < 2 THEN NULL ELSE date '2025-01-01' + fz.h(i, 12 + salt) % 400 END,
	       CASE WHEN fz.h(i, 13 + salt) % 100 < 5 THEN NULL ELSE fz.h(i, 14 + salt) % 1000 END,
	       CASE WHEN fz.h(i, 15 + salt) % 100 < 4 THEN NULL
	            ELSE ((fz.h(i, 16 + salt) % 50)::int8 * 100000000000 + fz.h(i, 17 + salt) % 1000) * CASE WHEN fz.h(i, 18 + salt) % 7 = 0 THEN -1 ELSE 1 END END,
	       CASE WHEN fz.h(i, 19 + salt) % 100 < 6 THEN NULL WHEN fz.h(i, 20 + salt) % 1000 = 0 THEN 'NaN'::float8
	            ELSE (fz.h(i, 21 + salt) % 10000) / 100.0 - 50 END,
	       CASE WHEN fz.h(i, 22 + salt) % 100 < 5 THEN NULL ELSE (fz.h(i, 23 + salt) % 100000) / 100.0 END,
	       CASE WHEN fz.h(i, 24 + salt) % 100 < 5 THEN NULL ELSE fz.h(i, 25 + salt) % 2 = 0 END,
	       CASE WHEN fz.h(i, 26 + salt) % 100 < 7 THEN NULL
	            WHEN fz.h(i, 27 + salt) % 50 = 0 THEN '{}'::jsonb
	            ELSE jsonb_build_object('parts', (SELECT coalesce(jsonb_agg(jsonb_build_object('cat', 'cat_' || (fz.h(i, 28 + salt) + j) % 6,
	                                                                                         'dir_a', (array['p', 'n'])[1 + (fz.h(i, 29 + salt) + j) % 2],
	                                                                                         'score', (fz.h(i, 30 + salt) * j) % 10)), '[]')
	                                              FROM generate_series(1, fz.h(i, 31 + salt) % 5) j)) END,
	       CASE WHEN fz.h(i, 32 + salt) % 100 < 6 THEN NULL
	            ELSE jsonb_build_object('m1', fz.h(i, 33 + salt) % 20, 'm2', (fz.h(i, 34 + salt) % 7) + 0.5,
	                                    'm3', CASE WHEN fz.h(i, 35 + salt) % 200 = 0 THEN to_jsonb('x'::text) WHEN fz.h(i, 35 + salt) % 200 = 1 THEN 'null'::jsonb
	                                               ELSE to_jsonb(fz.h(i, 36 + salt) % 5) END) END,
	       -- sort keys of byte strings: a uuid, a bytea of every length from 0 to 40 (some of them ties), titles that share their first 8 bytes and strings that are prefixes of others
	       CASE WHEN fz.h(i, 37 + salt) % 100 < 4 THEN NULL ELSE md5((fz.h(i, 38 + salt) % 3000)::text)::uuid END,
	       CASE WHEN fz.h(i, 39 + salt) % 100 < 4 THEN NULL ELSE decode(substr(md5((fz.h(i, 40 + salt) % 2500)::text) || md5('x'), 1, 2 * (fz.h(i, 40 + salt) % 41)), 'hex') END,
	       CASE WHEN fz.h(i, 41 + salt) % 100 < 4 THEN NULL WHEN fz.h(i, 41 + salt) % 100 < 7 THEN '' WHEN fz.h(i, 41 + salt) % 100 < 10 THEN 'item_' || fz.h(i, 42 + salt) % 4
	            ELSE 'item_' || substr(md5((fz.h(i, 42 + salt) % 4000)::text), 1, 3 + fz.h(i, 43 + salt) % 20) END,
	       -- sort keys of numbers the lanes hold in two words (more than 18 digits; a NaN is NULL plus a flag) and of a numeric image (spellings of one value, infinities)
	       CASE WHEN fz.h(i, 44 + salt) % 100 < 4 THEN NULL WHEN fz.h(i, 44 + salt) % 100 < 6 THEN 'NaN'::numeric
	            ELSE ((fz.h(i, 45 + salt) % 700)::numeric * 100000000000000 + fz.h(i, 46 + salt) % 9 / 10000.0) * CASE WHEN fz.h(i, 47 + salt) % 5 = 0 THEN -1 ELSE 1 END END,
	       CASE WHEN fz.h(i, 48 + salt) % 100 < 4 THEN NULL WHEN fz.h(i, 48 + salt) % 100 < 6 THEN 'NaN'::numeric WHEN fz.h(i, 48 + salt) % 100 < 7 THEN 'Infinity'::numeric
	            WHEN fz.h(i, 48 + salt) % 100 < 8 THEN '-Infinity'::numeric
	            ELSE round((fz.h(i, 49 + salt) % 400)::numeric / 8, fz.h(i, 50 + salt) % 5) * CASE WHEN fz.h(i, 51 + salt) % 6 = 0 THEN 100000000000000000000 ELSE 1 END END,
	       CASE WHEN fz.h(i, 52 + salt) % 100 < 5 THEN NULL ELSE (enum_range(NULL::fz.lvl))[1 + fz.h(i, 53 + salt) % 5] END,
	       -- an interval whose totals tie under different spellings (1 mon, 30 days, 720 hours), with infinities
	       CASE WHEN fz.h(i, 54 + salt) % 100 < 4 THEN NULL WHEN fz.h(i, 54 + salt) % 100 < 5 THEN 'infinity'::interval WHEN fz.h(i, 54 + salt) % 100 < 6 THEN '-infinity'::interval
	            ELSE CASE fz.h(i, 55 + salt) % 4 WHEN 0 THEN (fz.h(i, 56 + salt) % 30) * interval '1 month' WHEN 1 THEN (fz.h(i, 56 + salt) % 30) * interval '30 days'
	                                              WHEN 2 THEN (fz.h(i, 56 + salt) % 30) * interval '720 hours'
	                                              ELSE make_interval(0, fz.h(i, 57 + salt) % 4, 0, fz.h(i, 58 + salt) % 40, fz.h(i, 59 + salt) % 30, 0, fz.h(i, 60 + salt) % 100 / 4.0) END END
	FROM generate_series(lo, hi) i $$;
INSERT INTO events SELECT * FROM fz.gen_events(1, @ROWS@, 0);
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, grp, a, b, ts, amount, big, val, num, flag, doc, metrics, ref, blob, title, wnum, inum, lv, ivl)
	WITH (paths = 'doc.parts[]{cat text, dir_a text, score float8}, metrics.* float8');

-- first-row lookups: a unique key, a key some rows repeat, three versions of a key (unique with the version), versions with a NULL one
CREATE TABLE dims (a int, b int, label text, score int) WITH (autovacuum_enabled = false);
INSERT INTO dims SELECT x, y, 'l' || x || '_' || y, x * 3 + y FROM generate_series(0, 199) x, generate_series(0, 100) y WHERE fz.h(x * 101 + y, 40) % 10 <> 0;
CREATE UNIQUE INDEX dims_key ON dims (a, b);
CREATE TABLE dims_dup (a int, b int, label text, score int) WITH (autovacuum_enabled = false);
INSERT INTO dims_dup SELECT * FROM dims;
INSERT INTO dims_dup SELECT a, b, 'again', score + 1000 FROM dims WHERE a < 20;
CREATE INDEX dims_dup_key ON dims_dup (a, b);
CREATE TABLE vers (id int PRIMARY KEY, a int NOT NULL, b int NOT NULL, ts int, label text, score int) WITH (autovacuum_enabled = false);
INSERT INTO vers SELECT row_number() OVER (), x, y, CASE WHEN fz.h(x * 1000 + y * 7 + z, 41) % 5 = 0 THEN NULL ELSE z END, 'v' || x || '_' || y || '_' || z, x * 3 + y + z * 7
FROM (SELECT x, y, z FROM generate_series(0, 199) x, generate_series(0, 100) y, generate_series(1, 3) z WHERE fz.h(x * 101 + y, 42) % 10 <> 0
      ORDER BY fz.h(x * 1000 + y * 7 + z, 43)) q;
CREATE INDEX vers_key ON vers (a, b, ts);
CREATE TABLE dims2 (id int PRIMARY KEY, tag text, w int) WITH (autovacuum_enabled = false);
INSERT INTO dims2 SELECT i, 't' || i % 4, fz.h(i, 44) % 100 FROM generate_series(0, 299) i;
CREATE TABLE lookup (kind text, cat text, derived_cat text) WITH (autovacuum_enabled = false);
INSERT INTO lookup SELECT 'k' || k, 'cat_' || c, 'm' || (k + c) % 3 FROM generate_series(0, 4) k, generate_series(0, 5) c WHERE (k + c) % 5 <> 0;
CREATE TABLE lookup_dup (kind text, cat text, derived_cat text) WITH (autovacuum_enabled = false);
INSERT INTO lookup_dup SELECT * FROM lookup UNION ALL SELECT kind, cat, 'x' || derived_cat FROM lookup WHERE cat <> 'cat_2';
CREATE TABLE lookup_empty (kind text, cat text, derived_cat text);
CREATE TABLE buckets (kind text, ts date, label text) WITH (autovacuum_enabled = false);
INSERT INTO buckets VALUES ('k0', NULL, 'a'), ('k1', NULL, 'b'), ('k1', NULL, 'c'), (NULL, NULL, 'd'), ('k7', NULL, 'e'), ('k2', date '2025-02-03', 'f');

-- a hash partitioned table: the remainders are the partitions a key can be in
CREATE TABLE ev_part (id int, parent_id int, kind text, grp int, ts date, amount int, val float8) PARTITION BY HASH (id);
-- @heap
DO $$
BEGIN
	FOR i IN 0..@PARTS@ - 1 LOOP
		EXECUTE format('CREATE TABLE ev_part_%s PARTITION OF ev_part FOR VALUES WITH (MODULUS %s, REMAINDER %s) WITH (autovacuum_enabled = false)', i, @PARTS@, i);
	END LOOP;
END $$;
-- @end
-- row counts of a text dictionary (column format minor 24): s has about a tenth of the rows distinct values, with NULLs, empty strings, a wildcard character, and strings over 255 bytes in the rows the run adds (a segment of them has no counts)
CREATE TABLE dcf (id int, s text, n int) WITH (autovacuum_enabled = false, fillfactor = 10);
CREATE FUNCTION fz.gen_dcf(lo bigint, hi bigint, salt int) RETURNS SETOF dcf LANGUAGE sql AS $$
	SELECT i::int, CASE fz.h(i, 170 + salt) % 400 WHEN 0 THEN NULL WHEN 1 THEN '' WHEN 2 THEN 'a_b' || (fz.h(i, 171 + salt) % 50) WHEN 3 THEN '100%' || (fz.h(i, 172 + salt) % 50)
	       WHEN 4 THEN 'k' || md5(i::text) || CASE WHEN salt > 0 THEN repeat('z', 260) ELSE '' END
	       ELSE 'k' || substr(md5((fz.h(i, 173 + salt) % (@ROWS@ / 8 + 1))::text), 1, 4 + fz.h(i, 174 + salt) % 9) END,
	       fz.h(i, 175 + salt) % 1000
	FROM generate_series(lo, hi) i $$;
INSERT INTO dcf SELECT * FROM fz.gen_dcf(1, @ROWS@ * 2, 0);
CREATE INDEX dcf_idx ON dcf USING sift_col (id, s, n) WITH (count_columns = 's', write_buffer_limit = 4096);
-- key tables (column format minor 22): grp_id has 300 values, above the 256 groups of a unit's cube, over 40 times the rows of the other tables so that a table fits 3% of a segment
CREATE TABLE ktf (id int, grp_id int, ts timestamp, amt numeric(12,2), val int, pad text) WITH (autovacuum_enabled = false, fillfactor = 10);
CREATE FUNCTION fz.gen_ktf(lo bigint, hi bigint, salt int) RETURNS SETOF ktf LANGUAGE sql AS $$
	SELECT i::int, CASE WHEN fz.h(i, 160 + salt) % 50 = 0 THEN NULL ELSE fz.h(i, 161 + salt) % 300 - 40 END,
	       timestamp '2024-01-01' + (fz.h(i, 162 + salt) % 100000) * interval '7 minutes',
	       CASE WHEN fz.h(i, 163 + salt) % 20 = 0 THEN NULL ELSE (fz.h(i, 164 + salt) % 100000) / 100.0 - 300 END,
	       fz.h(i, 165 + salt) % 1000, md5(i::text) || md5((i + 1)::text)
	FROM generate_series(lo, hi) i $$;
INSERT INTO ktf SELECT * FROM fz.gen_ktf(1, @ROWS@ * 40, 0);
CREATE INDEX ktf_idx ON ktf USING sift_col (id, grp_id, ts, amt, val, pad);

-- @table
DO $$
BEGIN
	FOR i IN 0..@PARTS@ - 1 LOOP
		EXECUTE format('CREATE TABLE ev_part_%s PARTITION OF ev_part FOR VALUES WITH (MODULUS %s, REMAINDER %s) USING sift_table', i, @PARTS@, i);
	END LOOP;
END $$;
-- @end
CREATE FUNCTION fz.gen_part(lo bigint, hi bigint, salt int) RETURNS SETOF ev_part LANGUAGE sql AS $$
	SELECT id, parent_id, kind, grp, ts, amount, val FROM fz.gen_events(lo, hi, salt) $$;
INSERT INTO ev_part SELECT * FROM fz.gen_part(1, @ROWS@ / 2, 7);
CREATE INDEX ev_part_col ON ev_part USING sift_col (id, parent_id, kind, grp, ts, amount, val);
-- the lookups of the access method: a directory of heap addresses (point_tid_columns) beside the index above
CREATE INDEX ev_part_pt ON ev_part USING sift_col (id sift.col_int4_ops, parent_id sift.col_int4_ops, kind sift.col_text_ops, amount sift.col_int4_ops)
	WITH (point_tid_columns = 'id, parent_id, kind');

-- a key some units keep a bitmap of and others keep nothing of (a sparse key list is beyond the share of a unit), and a cube histogram of in all: the units
-- without it answer from the cube (a pad of 1500 bytes a row makes units of about 1,300 rows)
CREATE TABLE kc (id int, parent_id int, kind text, grp int, ts date, amount int, val float8, pad text) WITH (autovacuum_enabled = false);
ALTER TABLE kc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kc SELECT id, CASE WHEN id <= @ROWS@ / 3 THEN id % 150 ELSE (id % 150) * 1000003 END, kind, grp, ts, amount, val, repeat('x', 1500) FROM fz.gen_events(1, @ROWS@, 3);
CREATE INDEX kc_col ON kc USING sift_col (id, parent_id, kind, grp, ts, amount, val) WITH (preagg = 'cube(parent_id: amount)', key_columns = 'parent_id');

-- tables whose units are cut by a key, the rows in a heap order that is not the key order: integer, date, timestamp, numeric and float keys with NULLs,
-- infinities, NaN and a negative zero; 64 rows a unit aim, so that a table of a few thousand rows has dozens of units
CREATE TABLE kcut_a (id int, ki int, kd date, kt timestamp, kn numeric(14,3), kf float8, amount int, val float8, kind text, pad text) WITH (autovacuum_enabled = false);
CREATE FUNCTION fz.gen_kcut(lo bigint, hi bigint, salt int) RETURNS SETOF kcut_a LANGUAGE sql AS $$
	SELECT i::int,
	       CASE WHEN fz.h(i, 61 + salt) % 100 < 5 THEN NULL ELSE fz.h(i, 62 + salt) % 40 - 5 END,
	       CASE WHEN fz.h(i, 63 + salt) % 100 < 4 THEN NULL WHEN fz.h(i, 63 + salt) % 100 < 6 THEN 'infinity'::date WHEN fz.h(i, 63 + salt) % 100 < 8 THEN '-infinity'::date
	            ELSE date '2025-01-01' + fz.h(i, 64 + salt) % 200 END,
	       CASE WHEN fz.h(i, 65 + salt) % 100 < 4 THEN NULL WHEN fz.h(i, 65 + salt) % 100 < 6 THEN 'infinity'::timestamp WHEN fz.h(i, 65 + salt) % 100 < 8 THEN '-infinity'::timestamp
	            ELSE timestamp '2025-01-01' + (fz.h(i, 66 + salt) % 3000) * interval '1 hour' + (fz.h(i, 67 + salt) % 4) * interval '250 milliseconds' END,
	       CASE WHEN fz.h(i, 68 + salt) % 100 < 4 THEN NULL WHEN fz.h(i, 68 + salt) % 100 < 6 THEN 'NaN'::numeric
	            ELSE round((fz.h(i, 69 + salt) % 2000)::numeric / 8, fz.h(i, 70 + salt) % 4) * CASE WHEN fz.h(i, 71 + salt) % 9 = 0 THEN 1000000 ELSE 1 END END,
	       CASE WHEN fz.h(i, 72 + salt) % 100 < 4 THEN NULL WHEN fz.h(i, 72 + salt) % 100 < 6 THEN 'NaN'::float8 WHEN fz.h(i, 72 + salt) % 100 < 7 THEN 'Infinity'::float8
	            WHEN fz.h(i, 72 + salt) % 100 < 8 THEN '-Infinity'::float8 WHEN fz.h(i, 72 + salt) % 100 < 9 THEN '-0'::float8
	            ELSE (fz.h(i, 73 + salt) % 10000) / 100.0 - 50 END,
	       CASE WHEN fz.h(i, 74 + salt) % 100 < 5 THEN NULL ELSE fz.h(i, 75 + salt) % 1000 END,
	       CASE WHEN fz.h(i, 76 + salt) % 100 < 6 THEN NULL ELSE (fz.h(i, 77 + salt) % 10000) / 100.0 - 50 END,
	       CASE WHEN fz.h(i, 78 + salt) % 100 < 5 THEN NULL ELSE 'k' || fz.h(i, 79 + salt) % 5 END,
	       repeat('x', 60)
	FROM generate_series(lo, hi) i $$;
CREATE TABLE kcut_b (LIKE kcut_a) WITH (autovacuum_enabled = false);
CREATE TABLE kcut_c (LIKE kcut_a) WITH (autovacuum_enabled = false);
INSERT INTO kcut_a SELECT * FROM fz.gen_kcut(1, @ROWS@, 0);
INSERT INTO kcut_b SELECT * FROM fz.gen_kcut(1, @ROWS@, 20);
INSERT INTO kcut_c SELECT * FROM fz.gen_kcut(1, @ROWS@, 40);
-- keys of 8,192 rows or more under a second key, which a unit of 16,384 rows aims at: the units of a key are cut at 4,096 rows
CREATE TABLE kcut_d (LIKE kcut_a) WITH (autovacuum_enabled = false);
INSERT INTO kcut_d SELECT id, ki % 3, kd, kt, kn, kf, amount, val, kind, pad FROM fz.gen_kcut(1, @ROWS@ * 20, 60);
CREATE INDEX kcut_a_idx ON kcut_a USING sift_col (id, ki, kd, kt, kn, kf, amount, val, kind) WITH (key_cut = 'ki, kd', key_cut_rows = 64);
CREATE INDEX kcut_b_idx ON kcut_b USING sift_col (id, ki, kd, kt, kn, kf, amount, val, kind) WITH (key_cut = 'kt, kn', key_cut_rows = 64);
CREATE INDEX kcut_c_idx ON kcut_c USING sift_col (id, ki, kd, kt, kn, kf, amount, val, kind) WITH (key_cut = 'kf, ki', key_cut_rows = 64);
SET max_parallel_maintenance_workers = 0;	-- one segment, so that a key keeps its rows together
CREATE INDEX kcut_d_idx ON kcut_d USING sift_col (id, ki, kd, kt, kn, kf, amount, val, kind) WITH (key_cut = 'ki, kd', key_cut_rows = 16384);
RESET max_parallel_maintenance_workers;

-- a plain index over rows of 3 kB (two to a page, so a table of a few thousand rows has a dozen superblocks): the promotions that churn leaves are cut by TID range into units of the
-- floor
CREATE FUNCTION fz.gen_ufl(lo bigint, hi bigint, salt int) RETURNS SETOF kcut_a LANGUAGE sql AS $$
	SELECT id, ki, kd, kt, kn, kf, amount, val, kind, repeat('x', 3000) FROM fz.gen_kcut(lo, hi, salt) $$;
CREATE TABLE ufl (LIKE kcut_a) WITH (autovacuum_enabled = false);
ALTER TABLE ufl ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ufl SELECT * FROM fz.gen_ufl(1, @ROWS@, 80);
CREATE INDEX ufl_idx ON ufl USING sift_col (id, ki, kd, kt, kn, kf, amount, val, kind);

-- the same rows with cubes over the keys and a rollup of kind and ki: a unit with dead rows and every page visible is answered from them net of the dead rows (docs/COL-PREAGG.md 12)
CREATE TABLE udd (LIKE kcut_a) WITH (autovacuum_enabled = false);
ALTER TABLE udd ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO udd SELECT * FROM fz.gen_ufl(1, @ROWS@, 90);
CREATE INDEX udd_idx ON udd USING sift_col (id, ki, kd, kt, kn, kf, amount, val, kind)
	WITH (preagg = 'cube(kind: amount, val, kn, ki; ki: amount, val, kn; kd: amount, ki; kt: amount; kn: amount, ki)', rollup_dims = 'kind, ki: amount, kn', calendar_columns = 'kd, kt');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) udd;

-- units with a sorted run of the range leaves (order_columns): a range is answered from the run when it is short; a unit is 256 pages, so the rows of a few thousand fill one unit of more than 1,024 rows
CREATE TABLE kcut_w (LIKE kcut_a) WITH (autovacuum_enabled = false);
INSERT INTO kcut_w SELECT * FROM fz.gen_kcut(1, @ROWS@, 60);
CREATE INDEX kcut_w_idx ON kcut_w USING sift_col (id, ki, kd, kt, kn, kf, amount, val, kind) WITH (order_columns = 'ki, kd, kt, kn', cube_minmax = false, superunit = 0, calendar_columns = 'off');

-- NULL-heavy columns in runs of units (a unit is 256 pages, a row of 600 bytes a page): soft deletes (deleted_at is NULL in most rows and set in some runs and rows),
-- a date that is NULL for the first rows, a text and a numeric (NaN, NULLs) that are NULL in whole units and in single rows
CREATE TABLE nzf (id int, org int, amount int, val float8, kind text, deleted_at timestamptz, seen_at date, tag text, score numeric(10,2), pad text) WITH (autovacuum_enabled = false, fillfactor = 10);
CREATE FUNCTION fz.gen_nzf(lo bigint, hi bigint, salt int) RETURNS SETOF nzf LANGUAGE sql AS $$
	SELECT i::int, fz.h(i, 80 + salt) % 12,
	       CASE WHEN fz.h(i, 81 + salt) % 100 < 4 THEN NULL ELSE fz.h(i, 82 + salt) % 1000 END,
	       CASE WHEN fz.h(i, 83 + salt) % 100 < 6 THEN NULL ELSE (fz.h(i, 84 + salt) % 10000) / 100.0 - 50 END,
	       CASE WHEN fz.h(i, 85 + salt) % 100 < 5 THEN NULL ELSE 'k' || fz.h(i, 86 + salt) % 5 END,
	       CASE WHEN i / 256 % 5 = 2 AND fz.h(i, 87 + salt) % 4 > 0 THEN timestamptz '2025-06-01' + (fz.h(i, 88 + salt) % 1000) * interval '1 hour'
	            WHEN fz.h(i, 89 + salt) % 200 = 0 THEN timestamptz '2025-07-01' END,
	       CASE WHEN i / 256 % 4 = 0 THEN NULL WHEN fz.h(i, 90 + salt) % 50 = 0 THEN NULL ELSE date '2025-01-01' + fz.h(i, 91 + salt) % 300 END,
	       CASE WHEN i / 256 % 3 = 1 THEN NULL WHEN fz.h(i, 92 + salt) % 20 = 0 THEN NULL ELSE 't' || fz.h(i, 93 + salt) % 30 END,
	       CASE WHEN i / 256 % 6 = 3 THEN NULL WHEN i / 256 % 6 = 4 AND fz.h(i, 94 + salt) % 3 = 0 THEN 'NaN'::numeric
	            WHEN fz.h(i, 95 + salt) % 30 = 0 THEN NULL ELSE (fz.h(i, 96 + salt) % 4000) / 8.0 END,
	       repeat('x', 600)
	FROM generate_series(lo, hi) i $$;
INSERT INTO nzf SELECT * FROM fz.gen_nzf(1, @ROWS@, 0);
CREATE INDEX nzf_col ON nzf USING sift_col (id, org, amount, val, kind, deleted_at, seen_at, tag, score);

-- arrays of every length and shape (NULL, empty, NULL elements, two dimensions, another lower bound) in runs of 256 rows that are mostly NULL, mostly empty or mostly full:
-- the index keeps cardinality and array_length(x, 1) of each as two integer lanes, and the pushdown reads them for the length functions and the null tests
CREATE TABLE arf (id int, org int, kind text, amount int, ia int[], ta text[], tb text[], na numeric[], ma int[], pad text) WITH (autovacuum_enabled = false, fillfactor = 10);
CREATE FUNCTION fz.gen_arf(lo bigint, hi bigint, salt int) RETURNS SETOF arf LANGUAGE sql AS $$
	SELECT i::int, fz.h(i, 200 + salt) % 12,
	       CASE WHEN fz.h(i, 201 + salt) % 100 < 5 THEN NULL ELSE 'k' || fz.h(i, 202 + salt) % 5 END,
	       fz.h(i, 203 + salt) % 1000,
	       CASE WHEN i / 256 % 5 = 1 AND fz.h(i, 204 + salt) % 4 > 0 THEN NULL WHEN fz.h(i, 205 + salt) % 100 < 6 THEN NULL WHEN fz.h(i, 206 + salt) % 100 < 18 THEN '{}'
	            WHEN fz.h(i, 207 + salt) % 100 < 22 THEN ARRAY[NULL, NULL]::int[]
	            ELSE ARRAY(SELECT fz.h(i, 208 + k + salt) % 20 FROM generate_series(1, 1 + fz.h(i, 209 + salt) % 6) k) END,
	       CASE WHEN i / 256 % 4 = 2 THEN NULL WHEN i / 256 % 4 = 3 AND fz.h(i, 210 + salt) % 3 > 0 THEN '{}'::text[] WHEN fz.h(i, 211 + salt) % 100 < 9 THEN NULL
	            WHEN fz.h(i, 226 + salt) % 100 < 3 THEN ARRAY[['t1', 't2'], ['t' || fz.h(i, 227 + salt) % 30, 't4']]
	            WHEN fz.h(i, 228 + salt) % 100 < 3 THEN '[0:1]={t5,t6}'::text[]
	            WHEN fz.h(i, 229 + salt) % 100 < 3 THEN ARRAY['t1', NULL, 't' || fz.h(i, 230 + salt) % 30]
	            ELSE ARRAY(SELECT 't' || fz.h(i, 212 + k + salt) % 30 FROM generate_series(1, 1 + fz.h(i, 213 + salt) % 9) k) END,
	       CASE WHEN fz.h(i, 214 + salt) % 100 < 30 THEN NULL WHEN fz.h(i, 215 + salt) % 100 < 50 THEN '{}'::text[] ELSE ARRAY['u' || fz.h(i, 216 + salt) % 4, NULL, 'u0'] END,
	       CASE WHEN fz.h(i, 217 + salt) % 100 < 10 THEN NULL ELSE ARRAY[(fz.h(i, 218 + salt) % 3)::numeric, 1.50, (fz.h(i, 219 + salt) % 5) / 4.0] END,
	       CASE WHEN fz.h(i, 220 + salt) % 100 < 8 THEN NULL WHEN fz.h(i, 221 + salt) % 100 < 30 THEN ARRAY[[1, 2, 3], [4, 5, 6]] WHEN fz.h(i, 222 + salt) % 100 < 40 THEN ARRAY[[[1], [2]], [[3], [4]], [[5], [6]]]
	            WHEN fz.h(i, 223 + salt) % 100 < 55 THEN '[0:1]={7,8}'::int[] WHEN fz.h(i, 224 + salt) % 100 < 65 THEN '{}'::int[] ELSE ARRAY[fz.h(i, 225 + salt) % 9] END,
	       repeat('x', 300)
	FROM generate_series(lo, hi) i $$;
INSERT INTO arf SELECT * FROM fz.gen_arf(1, @ROWS@, 0);
-- the elements of ta, tb and ia are listed (paths) (ta and tb keep their images beside)
CREATE INDEX arf_col ON arf USING sift_col (id, org, kind, amount, ia, ta, tb, na, ma) WITH (paths = '+ta[] text, +tb[] text, ia[] int4');

-- tables joined on one column of events of each kind of leaf (dictionary code, int8, float8 with NaN, int, date), NULL keys and repeated keys included
CREATE TABLE ok_kind (k text, tag text, w int) WITH (autovacuum_enabled = false);
INSERT INTO ok_kind VALUES ('k0', 'a', 1), ('k1', 'b', 2), ('k1', 'c', 3), ('k3', 'd', 4), ('k9', 'e', 5), (NULL, 'f', 6);
CREATE TABLE ok_big (k int8, tag text, w int) WITH (autovacuum_enabled = false);
INSERT INTO ok_big SELECT big, 't' || id % 3, id % 11 FROM events WHERE id % 5 = 0 AND big IS NOT NULL;
INSERT INTO ok_big VALUES (NULL, 'n', 0);
CREATE TABLE ok_val (k float8, tag text, w int) WITH (autovacuum_enabled = false);
INSERT INTO ok_val SELECT DISTINCT ON (val) val, 't' || id % 4, id % 13 FROM events WHERE id % 4 = 0 ORDER BY val, id;
CREATE TABLE ok_grp (k int, tag text, w int) WITH (autovacuum_enabled = false);
INSERT INTO ok_grp SELECT g, 't' || g % 3, g FROM generate_series(0, 14) g WHERE g % 4 <> 1 UNION ALL VALUES (2, 'x', 20), (NULL, 'n', 0);
CREATE TABLE ok_ts (k date, tag text, w int) WITH (autovacuum_enabled = false);
INSERT INTO ok_ts SELECT date '2025-01-01' + d, 't' || d % 5, d % 17 FROM generate_series(0, 420, 3) d;

-- a soft-delete stamp NULL in all but a few rows of a unit (the exceptions of column format minor 19), floats of a wide spread, equal in some units, NULL now and then,
-- in the rows and in the elements of a map and of a list; a unit is 256 pages, a row a page
CREATE TABLE exf (id int, org int, kind text, amount int, val float8, dec float8, stamp timestamptz, metrics_a jsonb, parts jsonb, pad text) WITH (autovacuum_enabled = false, fillfactor = 10);
CREATE FUNCTION fz.gen_exf(lo bigint, hi bigint, salt int) RETURNS SETOF exf LANGUAGE sql AS $$
	SELECT i::int, fz.h(i, 100 + salt) % 7,
	       CASE WHEN fz.h(i, 101 + salt) % 100 < 4 THEN NULL ELSE 'k' || fz.h(i, 102 + salt) % 5 END,
	       CASE WHEN fz.h(i, 103 + salt) % 100 < 4 THEN NULL ELSE fz.h(i, 104 + salt) % 1000 END,
	       CASE WHEN i / 256 % 7 = 3 THEN 1.5 WHEN fz.h(i, 105 + salt) % 100 < 6 THEN NULL
	            ELSE (fz.h(i, 106 + salt) % 2001 - 1000) * power(2, fz.h(i, 107 + salt) % 11 - 5)::float8 / 4 END,
	       CASE WHEN fz.h(i, 108 + salt) % 100 < 6 THEN NULL ELSE (fz.h(i, 109 + salt) % 10000) / 100.0 - 50 END,
	       CASE WHEN i / 256 % 5 = 1 AND fz.h(i, 110 + salt) % 60 = 0 THEN timestamptz '2025-06-01' + (fz.h(i, 111 + salt) % 100) * interval '1 day'
	            WHEN i / 256 % 5 = 4 AND fz.h(i, 112 + salt) % 2 = 0 THEN timestamptz '2025-07-01'
	            WHEN fz.h(i, 113 + salt) % 400 = 0 THEN timestamptz '2025-08-01' END,
	       CASE WHEN fz.h(i, 114 + salt) % 100 < 5 THEN NULL
	            ELSE jsonb_build_object('a', (fz.h(i, 115 + salt) % 2001 - 1000) * power(2, fz.h(i, 116 + salt) % 11 - 5)::float8 / 4, 'b', (fz.h(i, 117 + salt) % 10000) / 100.0 - 50)
	                 || CASE WHEN fz.h(i, 118 + salt) % 3 = 0 THEN jsonb_build_object('c', 2.5) ELSE '{}' END END,
	       CASE WHEN fz.h(i, 119 + salt) % 100 < 5 THEN NULL
	            ELSE jsonb_build_array(jsonb_build_object('cat', 'c' || fz.h(i, 120 + salt) % 4, 'score', (fz.h(i, 121 + salt) % 10000) / 100.0 - 50),
	                                   jsonb_build_object('cat', 'c' || fz.h(i, 122 + salt) % 4, 'score', (fz.h(i, 123 + salt) % 2001 - 1000) * power(2, fz.h(i, 124 + salt) % 11 - 5)::float8 / 4)) END,
	       repeat('x', 600)
	FROM generate_series(lo, hi) i $$;
INSERT INTO exf SELECT * FROM fz.gen_exf(1, @ROWS@, 0);
CREATE INDEX exf_col ON exf USING sift_col (id, org, kind, amount, val, dec, stamp, metrics_a, parts) WITH (paths = 'metrics_a.* float8, parts[]{cat text, score float8}', superunit = 0);

-- @heap
-- Reusing an empty overflow bitmap must not omit exceptions or select another leaf.
CREATE TABLE exr (id int, org int, kind int, amount bigint, marker jsonb, stamp bigint);
INSERT INTO exr
SELECT i, (i - 1) / 100, i % 10, i * 100,
       CASE WHEN i > 100 AND i <= 200 THEN '{"x":"bad"}'::jsonb
            WHEN i % 100 = 0 THEN '{"x":1}'::jsonb ELSE '{}'::jsonb END,
       CASE WHEN i % 100 = 99 THEN 1 END
FROM generate_series(1, 1000) i;
CREATE INDEX exr_col ON exr USING sift_col (id, org, kind, amount, marker, stamp)
  WITH (key_cut = 'org', key_cut_rows = 100, paths = 'marker.x bigint', superunit = 0);
VACUUM (FREEZE, ANALYZE) exr;
-- @end

-- ORDER BY leaves, then columns the index does not hold (TOPN_TIES, docs/USAGE.md): ties and NULLs in the leaves, and updates that make a tie group the statistics do not know
CREATE TABLE trf (id int, k int, d date, t timestamp, w bigint, x float8, s text, pad text) WITH (autovacuum_enabled = false);
CREATE FUNCTION fz.gen_trf(lo bigint, hi bigint, salt int) RETURNS SETOF trf LANGUAGE sql AS $$
	SELECT i::int,
	       CASE WHEN fz.h(i, 150 + salt) % 100 < 5 THEN NULL ELSE fz.h(i, 151 + salt) % 300 END,
	       CASE WHEN fz.h(i, 152 + salt) % 100 < 5 THEN NULL ELSE date '2024-01-01' + fz.h(i, 153 + salt) % 120 END,
	       CASE WHEN fz.h(i, 154 + salt) % 100 < 5 THEN NULL ELSE timestamp '2025-01-01' + (fz.h(i, 155 + salt) % 4000) * interval '1 second' END,
	       CASE WHEN fz.h(i, 156 + salt) % 100 < 8 THEN NULL ELSE fz.h(i, 157 + salt) % 1000 END,
	       CASE WHEN fz.h(i, 158 + salt) % 100 < 8 THEN NULL ELSE (fz.h(i, 159 + salt) % 50) - 25.0 END,
	       CASE WHEN fz.h(i, 160 + salt) % 100 < 8 THEN NULL ELSE md5((fz.h(i, 161 + salt) % 97)::text) END,
	       repeat('x', 300)
	FROM generate_series(lo, hi) i $$;
INSERT INTO trf SELECT * FROM fz.gen_trf(1, @ROWS@, 0);
CREATE INDEX trf_idx ON trf USING sift_col (id sift.col_int4_ops, k sift.col_int4_ops, d sift.col_date_ops, t sift.col_timestamp_ops);
-- keys of every type the global order covers, with ties and NULLs, read by a single-key ORDER BY .. LIMIT: the rows of the units in the heap order of the keys
CREATE TABLE gof (id int, a int2, b int, c int8, d date, t timestamp, z timestamptz, pad text) WITH (autovacuum_enabled = false);
CREATE FUNCTION fz.gen_gof(lo bigint, hi bigint, salt int) RETURNS SETOF gof LANGUAGE sql AS $$
	SELECT i::int,
	       CASE WHEN fz.h(i, 130 + salt) % 100 < 5 THEN NULL ELSE (fz.h(i, 131 + salt) % 200 - 100)::int2 END,
	       CASE WHEN fz.h(i, 132 + salt) % 100 < 5 THEN NULL ELSE fz.h(i, 133 + salt) % 700 END,
	       CASE WHEN fz.h(i, 134 + salt) % 100 < 5 THEN NULL ELSE (fz.h(i, 135 + salt) % 20000)::int8 * 100000 - 1000000000 END,
	       CASE WHEN fz.h(i, 136 + salt) % 100 < 5 THEN NULL ELSE date '2024-01-01' + fz.h(i, 137 + salt) % 300 END,
	       CASE WHEN fz.h(i, 138 + salt) % 100 < 5 THEN NULL ELSE timestamp '2025-01-01' + (fz.h(i, 139 + salt) % 50000) * interval '1 second' END,
	       CASE WHEN fz.h(i, 140 + salt) % 100 < 5 THEN NULL ELSE timestamptz '2025-01-01+00' + (fz.h(i, 141 + salt) % 60000) * interval '1 second' END,
	       repeat('x', 300)
	FROM generate_series(lo, hi) i $$;
INSERT INTO gof SELECT * FROM fz.gen_gof(1, @ROWS@, 0);
CREATE INDEX gof_idx ON gof USING sift_col (id sift.col_int4_ops, a sift.col_int2_ops, b sift.col_int4_ops, c sift.col_int8_ops, d sift.col_date_ops, t sift.col_timestamp_ops,
	z sift.col_timestamptz_ops) WITH (order_columns = 'id, a, b, c, d, t, z', order_array_columns = 'id, a, b, c, d, t, z', point_tid_columns = 'b');
-- ... and the same rows with arrays grouped by a column (order_array_group): WHERE a = x ORDER BY b / t LIMIT n reads the entries of the group
CREATE TABLE gog (LIKE gof) WITH (autovacuum_enabled = false);
INSERT INTO gog SELECT * FROM fz.gen_gof(1, @ROWS@, 0);
CREATE INDEX gog_idx ON gog USING sift_col (id sift.col_int4_ops, a sift.col_int2_ops, b sift.col_int4_ops, t sift.col_timestamp_ops) WITH (order_columns = 'b, t', order_array_columns = 'b, t', order_array_group = 'a');

-- instants of a time zone's clock changes (the weeks around the daylight saving time of the northern and the southern zones, the days of the reform, local mean time),
-- the infinities and NULL, grouped by the day or hour of the session's zone
CREATE TABLE zkf (id int, org int, z timestamptz, amount int, pad text) WITH (autovacuum_enabled = false, fillfactor = 10);
CREATE FUNCTION fz.gen_zkf(lo bigint, hi bigint, salt int) RETURNS SETOF zkf LANGUAGE sql AS $$
	SELECT i::int, fz.h(i, 150 + salt) % 5,
	       CASE fz.h(i, 151 + salt) % 400
	            WHEN 0 THEN NULL WHEN 1 THEN 'infinity' WHEN 2 THEN '-infinity'
	            ELSE (ARRAY[timestamptz '2024-03-09+00', '2024-11-02+00', '2024-03-31+00', '2024-10-05+00', '2024-04-06+00', '1883-11-17+00', '1582-10-01+00', '2018-02-03+00', '2100-02-27+00'])[1 + fz.h(i, 152 + salt) % 9]
	                 + (fz.h(i, 153 + salt) % 4000) * interval '41 minutes 13 seconds' END,
	       fz.h(i, 154 + salt) % 100,
	       repeat('x', 200)
	FROM generate_series(lo, hi) i $$;
INSERT INTO zkf SELECT * FROM fz.gen_zkf(1, @ROWS@, 0);
CREATE INDEX zkf_idx ON zkf USING sift_col (id sift.col_int4_ops, org sift.col_int4_ops, z sift.col_timestamptz_ops, amount sift.col_int4_ops);

-- @table
-- the fact tables rewritten as sift_tables (the --table mode of the driver, which also drops the sift_col indexes above): the jsonb paths of those indexes are
-- declared per column, a VACUUM FULL writes their leaves; ev_part has sift_table partitions
ALTER TABLE events SET ACCESS METHOD sift_table;
SELECT sift.table_set_paths('events', 'doc', 'doc.parts[]{cat text, dir_a text, score float8}');
SELECT sift.table_set_paths('events', 'metrics', 'metrics.* float8');
VACUUM FULL events;
ALTER TABLE kc SET ACCESS METHOD sift_table;
ALTER TABLE nzf SET ACCESS METHOD sift_table;
ALTER TABLE exf SET ACCESS METHOD sift_table;
SELECT sift.table_set_paths('exf', 'metrics_a', 'metrics_a.* float8');
SELECT sift.table_set_paths('exf', 'parts', 'parts[]{cat text, score float8}');
VACUUM FULL exf;
-- @end

-- COPY of the rows a generator makes (a superuser: through a file of the data directory); a table without a unique index takes it as a group of columns
CREATE FUNCTION fz.bulk_insert(tbl regclass, gen text, lo bigint, hi bigint, salt int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE f text := current_setting('data_directory') || '/fz_bulk.dat';
BEGIN
	EXECUTE format('COPY (SELECT * FROM fz.%I(%s, %s, %s)) TO %L', gen, lo, hi, salt, f);
	EXECUTE format('COPY %s FROM %L', tbl, f);
END $$;
-- @heap
-- A transaction that rolls back, read after each step by the plan of the session (the pushdown, or the heap with sift.enable_agg off): rows of one
-- statement, one by one, COPY, a savepoint that rolls back, an ON CONFLICT insert half of whose rows exist, new row versions, deletes. The rows a backend
-- has written in its own transaction must be in what it reads at once, whatever the way they got into the write buffer.
CREATE FUNCTION fz.txn_mix(lo bigint, n int, salt int, ops text) RETURNS TABLE (step int, res text) LANGUAGE plpgsql AS $$
DECLARE c text; nxt bigint := lo; r text[] := '{}'; s text;
BEGIN
	BEGIN
		FOREACH c IN ARRAY string_to_array(ops, ',') LOOP
			IF c = 'i' THEN
				INSERT INTO events SELECT * FROM fz.gen_events(nxt, nxt + n - 1, salt);
			ELSIF c = 'o' THEN
				FOR j IN 0..n - 1 LOOP INSERT INTO events SELECT * FROM fz.gen_events(nxt + j, nxt + j, salt); END LOOP;
			ELSIF c = 'c' THEN
				PERFORM fz.bulk_insert('events', 'gen_events', nxt, nxt + n - 1, salt);
			ELSIF c = 's' THEN
				BEGIN
					INSERT INTO events SELECT * FROM fz.gen_events(nxt, nxt + n - 1, salt);
					RAISE EXCEPTION 'undo';
				EXCEPTION WHEN raise_exception THEN NULL;
				END;
			ELSIF c = 'x' THEN
				INSERT INTO events SELECT * FROM fz.gen_events(nxt - n / 2, nxt + n - 1, salt) ON CONFLICT (id) DO NOTHING;
			ELSIF c = 'u' THEN
				UPDATE events SET amount = coalesce(amount, 0) + 1, grp = (coalesce(grp, 0) + 1) % 12 WHERE id >= nxt - n AND id < nxt + n;
			ELSIF c = 'd' THEN
				DELETE FROM events WHERE id >= nxt - n AND id < nxt - n / 2;
			END IF;
			nxt := nxt + n;
			SELECT string_agg(coalesce(g::text, 'n') || ':' || k || ':' || m, ',' ORDER BY g NULLS FIRST) INTO s
			FROM (SELECT grp g, count(*) k, sum(amount) m FROM events GROUP BY grp) x;
			r := r || (s || '/' || (SELECT count(*) || ':' || coalesce(max(id), -1) FROM events));
		END LOOP;
		RAISE EXCEPTION 'rolled back';
	EXCEPTION WHEN raise_exception THEN NULL;
	END;
	RETURN QUERY SELECT g, r[g] FROM generate_series(1, coalesce(array_length(r, 1), 0)) g;
END $$;
-- @end

-- the group shapes: the keys of a CTE are picked from a table with a column index through the links of a group (sizes 0, 1, 5, 60, 200 and all 600 members; one NULL member)
-- and the assignments (a row with a NULL member stands for the links of its group), then the events of those keys are grouped by the label a small table maps
-- their metrics to (some of them unmapped, some mapped to NULL)
-- @heap
CREATE TABLE fk_members (id int PRIMARY KEY, kind text, name text) WITH (autovacuum_enabled = false);
INSERT INTO fk_members SELECT i, CASE WHEN fz.h(i, 160) % 20 = 0 THEN NULL ELSE 'k' || fz.h(i, 161) % 3 END, CASE WHEN fz.h(i, 162) % 5 = 0 THEN NULL ELSE 'n' || i END FROM generate_series(0, 599) i;
CREATE INDEX fk_members_col ON fk_members USING sift_col (id, kind, name);
CREATE TABLE fk_links (grp int, dim_b_id int) WITH (autovacuum_enabled = false);
INSERT INTO fk_links SELECT g - 1, (i * 7) % 600 FROM unnest(ARRAY[0, 1, 5, 60, 200, 600]) WITH ORDINALITY s(n, g), generate_series(0, 599) i WHERE i < n
UNION ALL SELECT 1, 11 UNION ALL SELECT 4, NULL;
CREATE TABLE fk_assign (grp int, dim_b_id int, sub int) WITH (autovacuum_enabled = false);
INSERT INTO fk_assign SELECT g, NULL, 9 FROM generate_series(0, 6) g UNION ALL SELECT g, fz.h(g, 163) % 600, 8 FROM generate_series(0, 6) g UNION ALL SELECT 2, NULL, 8;
CREATE TABLE fk_map (src int, key text, label text) WITH (autovacuum_enabled = false);
INSERT INTO fk_map SELECT s, 'm' || k, CASE WHEN fz.h(s * 4 + k, 164) % 4 = 0 THEN NULL ELSE 'lab' || (fz.h(s * 4 + k, 165) % 3) END FROM generate_series(0, 11) s, generate_series(1, 3) k WHERE fz.h(s * 4 + k, 166) % 3 <> 0;
VACUUM (FREEZE, ANALYZE) fk_members, fk_links, fk_assign, fk_map;
-- @end

-- the key lists: facts with a column index and a btree, the entities of a scope listed in a links table (scopes of 0, 1, 5, 60, 200 and all 300 entities; one lists an entity twice and a NULL one),
-- a small table of units with a NULL flag and one of origins
-- @heap
CREATE TABLE kl_units (id int PRIMARY KEY, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO kl_units SELECT i, CASE WHEN i % 9 = 0 THEN true WHEN i % 11 = 0 THEN NULL ELSE false END FROM generate_series(1, 60) i;
CREATE TABLE kl_origins (id int PRIMARY KEY, slug text) WITH (autovacuum_enabled = false);
INSERT INTO kl_origins SELECT i, 'o' || i FROM generate_series(1, 8) i;
CREATE TABLE kl_links (grp int, dim_a_id int) WITH (autovacuum_enabled = false);
INSERT INTO kl_links SELECT g - 1, (i * 7) % 300 FROM unnest(ARRAY[0, 1, 5, 60, 200, 300]) WITH ORDINALITY s(n, g), generate_series(0, 299) i WHERE i < n
UNION ALL SELECT 6, 3 UNION ALL SELECT 6, 3 UNION ALL SELECT 6, 4 UNION ALL SELECT 6, NULL;
CREATE TABLE kl_ev (id int PRIMARY KEY, dim_a_id int, unit_id int, foreign_id int, day date, price float8, latest bool, ccy text) WITH (autovacuum_enabled = false);
INSERT INTO kl_ev SELECT i, CASE WHEN fz.h(i, 170) % 40 = 0 THEN NULL WHEN fz.h(i, 171) % 3 = 0 THEN fz.h(i, 172) % 20 ELSE fz.h(i, 173) % 290 END,
       CASE WHEN fz.h(i, 174) % 30 = 0 THEN NULL ELSE 1 + fz.h(i, 175) % 60 END, 1 + fz.h(i, 176) % 8, date '2025-01-01' + fz.h(i, 177) % 450,
       CASE WHEN fz.h(i, 178) % 20 = 0 THEN NULL ELSE 10 + fz.h(i, 179) % 90 END, fz.h(i, 180) % 4 <> 0, CASE fz.h(i, 181) % 5 WHEN 0 THEN NULL WHEN 1 THEN 'eur' ELSE 'usd' END
FROM generate_series(1, 6000) i;
CREATE INDEX kl_ev_entity_day ON kl_ev (dim_a_id, day);
CREATE INDEX kl_ev_col ON kl_ev USING sift_col (id, dim_a_id, unit_id, foreign_id, day, price, latest, ccy);
VACUUM (FREEZE, ANALYZE) kl_units, kl_origins, kl_links, kl_ev;
-- @end

-- the shared CTE copies: documents with a column index (their members are read by two aggregates) and a heavier table that a statement joins 1 to 3 times
-- @heap
CREATE TABLE cc_docs (id int PRIMARY KEY, grp int NOT NULL, v int NOT NULL, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO cc_docs SELECT i, i % 2000, i % 17, CASE WHEN fz.h(i, 190) % 15 = 0 THEN NULL ELSE jsonb_build_object('a', i % 9, 'b', i % 5, 'c', i % 3) END FROM generate_series(1, 8000) i;
CREATE INDEX cc_docs_col ON cc_docs USING sift_col (id, grp, v, doc) WITH (paths = 'doc.* float8');
CREATE TABLE cc_fat (id int PRIMARY KEY, grp int, w int) WITH (autovacuum_enabled = false);
INSERT INTO cc_fat SELECT i, i % 50, i % 1000 FROM generate_series(1, 6000) i;
VACUUM (FREEZE, ANALYZE) cc_docs, cc_fat;
-- @end

VACUUM (FREEZE, ANALYZE) dcf, items, events, ok_kind, ok_big, ok_val, ok_grp, ok_ts, ev_part, kc, dims, vers, dims2, lookup, lookup_dup, buckets, dims_dup, nzf, arf, exf, trf, gof, gog, zkf, ktf, kcut_a, kcut_b, kcut_c, kcut_d;
