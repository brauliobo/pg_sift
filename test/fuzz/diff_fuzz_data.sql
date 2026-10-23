-- Data of the differential fuzz (test/fuzz/diff_fuzz.py): small tables with NULLs and duplicates, every row a function of its id and the seed.
-- @SEED@ @ROWS@ @PARTS@ are replaced by the driver.
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE SCHEMA fz;
CREATE FUNCTION fz.h(i bigint, salt int) RETURNS int IMMUTABLE PARALLEL SAFE LANGUAGE sql AS
$$ SELECT ((((i + salt::bigint * 1000003 + @SEED@::bigint * 7919) * 2654435761) % 4294967296) / 65536)::int $$;

CREATE TABLE items (id int PRIMARY KEY, kind text, enabled bool, grp int) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT i, 'k' || fz.h(i, 90) % 5, fz.h(i, 91) % 7 <> 3, CASE WHEN fz.h(i, 92) % 10 = 0 THEN NULL ELSE fz.h(i, 93) % 12 END
FROM generate_series(0, 599) i;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, grp int, a int, b int, ts date, amount int, big int8, val float8, num numeric(12,2),
                     flag bool, doc jsonb, metrics jsonb, ref uuid, blob bytea, title text) WITH (autovacuum_enabled = false, fillfactor = 75);
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
	            ELSE 'item_' || substr(md5((fz.h(i, 42 + salt) % 4000)::text), 1, 3 + fz.h(i, 43 + salt) % 20) END
	FROM generate_series(lo, hi) i $$;
INSERT INTO events SELECT * FROM fz.gen_events(1, @ROWS@, 0);
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, grp, a, b, ts, amount, big, val, num, flag, doc, metrics, ref, blob, title)
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
DO $$
BEGIN
	FOR i IN 0..@PARTS@ - 1 LOOP
		EXECUTE format('CREATE TABLE ev_part_%s PARTITION OF ev_part FOR VALUES WITH (MODULUS %s, REMAINDER %s) WITH (autovacuum_enabled = false)', i, @PARTS@, i);
	END LOOP;
END $$;
CREATE FUNCTION fz.gen_part(lo bigint, hi bigint, salt int) RETURNS SETOF ev_part LANGUAGE sql AS $$
	SELECT id, parent_id, kind, grp, ts, amount, val FROM fz.gen_events(lo, hi, salt) $$;
INSERT INTO ev_part SELECT * FROM fz.gen_part(1, @ROWS@ / 2, 7);
CREATE INDEX ev_part_col ON ev_part USING sift_col (id, parent_id, kind, grp, ts, amount, val);

-- a key some units keep a bitmap of and others keep nothing of (a sparse key list is beyond the share of a unit), and a cube histogram of in all: the units
-- without it answer from the cube (a pad of 1500 bytes a row makes units of about 1,300 rows)
CREATE TABLE kc (id int, parent_id int, kind text, grp int, ts date, amount int, val float8, pad text) WITH (autovacuum_enabled = false);
ALTER TABLE kc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kc SELECT id, CASE WHEN id <= @ROWS@ / 3 THEN id % 150 ELSE (id % 150) * 1000003 END, kind, grp, ts, amount, val, repeat('x', 1500) FROM fz.gen_events(1, @ROWS@, 3);
CREATE INDEX kc_col ON kc USING sift_col (id, parent_id, kind, grp, ts, amount, val) WITH (preagg = 'cube(parent_id: amount)', key_columns = 'parent_id');

VACUUM (FREEZE, ANALYZE) items, events, ev_part, kc, dims, vers, dims2, lookup, lookup_dup, buckets, dims_dup;
