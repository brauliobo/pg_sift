-- UNIQUE sift_col (format major 2) with the registry of the keys a transaction holds in its stage: the typed operator classes are the default of their types (ON CONFLICT with no class written), a
-- duplicate of a row of the same transaction or of one flushed to the partitions is 23505 whatever the length of the key, the entries of a savepoint that rolled back leave nothing, and a
-- transaction that writes more keys than the registry holds sends its stage to the partitions and goes on.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

-- the class of an index made with no class written: the typed one of the type (varchar takes the one of text), sift.col_ops for the types whose class takes some columns only (numeric, char(n)) and for those that have none
CREATE TABLE ops_t (i2 smallint, i4 int, i8 bigint, d date, ts timestamp, tz timestamptz, b bool, n numeric, n2 numeric(30, 2), t text, v varchar(40), c char(4), u uuid, f4 real, f8 float8, j jsonb, y bytea, a int[]);
CREATE INDEX ops_i ON ops_t USING sift_col (i2, i4, i8, d, ts, tz, b, n, n2, t, v, c, u, f4, f8, j, y, a);
SELECT a.attname, oc.opcname FROM pg_index x JOIN pg_attribute a ON a.attrelid = x.indexrelid JOIN pg_opclass oc ON oc.oid = x.indclass[a.attnum - 1]
WHERE x.indexrelid = 'ops_i'::regclass ORDER BY a.attnum;
SELECT count(*) AS default_classes FROM pg_opclass WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'sift_col') AND opcdefault;

-- ON CONFLICT with no class written, for each type a key can be
CREATE TABLE oc_t (i2 smallint, i4 int, i8 bigint, d date, ts timestamp, tz timestamptz, b bool, n numeric(12, 2), t text, v varchar(40), u uuid, k1 int, k2 text, hits int DEFAULT 1);
CREATE UNIQUE INDEX oc_i2 ON oc_t USING sift_col (i2);
CREATE UNIQUE INDEX oc_i4 ON oc_t USING sift_col (i4);
CREATE UNIQUE INDEX oc_i8 ON oc_t USING sift_col (i8);
CREATE UNIQUE INDEX oc_d ON oc_t USING sift_col (d);
CREATE UNIQUE INDEX oc_ts ON oc_t USING sift_col (ts);
CREATE UNIQUE INDEX oc_tz ON oc_t USING sift_col (tz);
CREATE UNIQUE INDEX oc_b ON oc_t USING sift_col (b);
CREATE UNIQUE INDEX oc_n ON oc_t USING sift_col (n sift.col_numeric_ops);
CREATE UNIQUE INDEX oc_tx ON oc_t USING sift_col (t);
CREATE UNIQUE INDEX oc_v ON oc_t USING sift_col (v);
CREATE UNIQUE INDEX oc_u ON oc_t USING sift_col (u);
CREATE UNIQUE INDEX oc_k ON oc_t USING sift_col (k1, k2);
-- one key at a time: the other columns hold NULL, which is distinct
INSERT INTO oc_t (i2) VALUES (1) ON CONFLICT (i2) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (i2) VALUES (1) ON CONFLICT (i2) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (i4) VALUES (1) ON CONFLICT (i4) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (i4) VALUES (1) ON CONFLICT (i4) DO NOTHING;
INSERT INTO oc_t (i8) VALUES (1) ON CONFLICT (i8) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (i8) VALUES (1) ON CONFLICT (i8) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (d) VALUES ('2024-01-01') ON CONFLICT (d) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (d) VALUES ('2024-01-01') ON CONFLICT (d) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (ts) VALUES ('2024-01-01 10:00') ON CONFLICT (ts) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (ts) VALUES ('2024-01-01 10:00') ON CONFLICT (ts) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (tz) VALUES ('2024-01-01 10:00+00') ON CONFLICT (tz) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (tz) VALUES ('2024-01-01 10:00+00') ON CONFLICT (tz) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (b) VALUES (true) ON CONFLICT (b) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (b) VALUES (true) ON CONFLICT (b) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (n) VALUES (1.0) ON CONFLICT (n) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (n) VALUES (1.00) ON CONFLICT (n) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (t) VALUES ('short') ON CONFLICT (t) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (t) VALUES ('short') ON CONFLICT (t) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (v) VALUES ('a varchar key longer than sixteen bytes') ON CONFLICT (v) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (v) VALUES ('a varchar key longer than sixteen bytes') ON CONFLICT (v) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (u) VALUES ('00000000-0000-0000-0000-000000000001') ON CONFLICT (u) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (u) VALUES ('00000000-0000-0000-0000-000000000001') ON CONFLICT (u) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (k1, k2) VALUES (1, 'x') ON CONFLICT (k1, k2) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (k1, k2) VALUES (1, 'x') ON CONFLICT (k1, k2) DO UPDATE SET hits = oc_t.hits + 1;
INSERT INTO oc_t (k1, k2) VALUES (1, 'y') ON CONFLICT (k1, k2) DO NOTHING;
SELECT count(*) AS rows, sum(hits) AS hits FROM oc_t;
SELECT coalesce(i2::text, i4::text, i8::text, d::text, ts::text, tz::text, b::text, n::text, t, v, u::text, k1 || k2) AS key, hits FROM oc_t ORDER BY 2, 1;

-- a duplicate within a transaction, in the same statement, after a delete and with a changed key
CREATE TABLE dup_t (id int, v text);
CREATE UNIQUE INDEX dup_i ON dup_t USING sift_col (id);
BEGIN;
INSERT INTO dup_t VALUES (1, 'a');
INSERT INTO dup_t VALUES (1, 'b');
ROLLBACK;
INSERT INTO dup_t VALUES (2, 'a'), (3, 'b'), (2, 'c');
BEGIN;
INSERT INTO dup_t VALUES (10, 'a');
DELETE FROM dup_t WHERE id = 10;
INSERT INTO dup_t VALUES (10, 'again');
UPDATE dup_t SET v = 'updated' WHERE id = 10;
UPDATE dup_t SET id = 11 WHERE id = 10;
INSERT INTO dup_t VALUES (10, 'vacated');
COMMIT;
SELECT id, v FROM dup_t ORDER BY id;
INSERT INTO dup_t VALUES (11, 'committed duplicate');
INSERT INTO dup_t VALUES (10, 'committed duplicate');
-- the same key many times over in one transaction: the entries of one key are flushed, not piled up
BEGIN;
INSERT INTO dup_t VALUES (20, 'a');
UPDATE dup_t SET v = 'b' WHERE id = 20;
UPDATE dup_t SET v = 'c' WHERE id = 20;
UPDATE dup_t SET v = 'd' WHERE id = 20;
UPDATE dup_t SET v = 'e' WHERE id = 20;
UPDATE dup_t SET v = 'f' WHERE id = 20;
UPDATE dup_t SET v = 'g' WHERE id = 20;
UPDATE dup_t SET v = 'h' WHERE id = 20;
UPDATE dup_t SET v = 'i' WHERE id = 20;
UPDATE dup_t SET v = 'j' WHERE id = 20;
UPDATE dup_t SET v = 'k' WHERE id = 20;
UPDATE dup_t SET v = 'l' WHERE id = 20;
INSERT INTO dup_t VALUES (20, 'dup');
ROLLBACK;
SELECT count(*) FROM dup_t WHERE id = 20;

-- a savepoint that rolled back holds no key; one that was released keeps it
BEGIN;
SAVEPOINT s1;
INSERT INTO dup_t VALUES (30, 'in s1');
ROLLBACK TO s1;
INSERT INTO dup_t VALUES (30, 'after the rollback');
SAVEPOINT s2;
INSERT INTO dup_t VALUES (31, 'in s2');
RELEASE s2;
INSERT INTO dup_t VALUES (31, 'duplicate of the released one');
ROLLBACK;
BEGIN;
SAVEPOINT s1;
INSERT INTO dup_t VALUES (40, 'in s1');
SAVEPOINT s2;
INSERT INTO dup_t VALUES (41, 'in s2');
ROLLBACK TO s2;
INSERT INTO dup_t VALUES (40, 'duplicate of s1');
ROLLBACK TO s1;
INSERT INTO dup_t VALUES (40, 'ok now');
INSERT INTO dup_t VALUES (41, 'ok now');
COMMIT;
SELECT id, v FROM dup_t WHERE id >= 30 ORDER BY id;
-- a failed statement in a block with an exception leaves its keys free
DO $$ BEGIN
	BEGIN
		INSERT INTO dup_t VALUES (50, 'a'), (51, 'b'), (50, 'c');
	EXCEPTION WHEN unique_violation THEN
		RAISE NOTICE 'caught';
	END;
	INSERT INTO dup_t VALUES (50, 'a'), (51, 'b');
END $$;
SELECT id, v FROM dup_t WHERE id >= 50 ORDER BY id;

-- keys that are equal by their value and not by their bytes, keys of 16 and 17 bytes, long keys, the NULLs of NULLS NOT DISTINCT
CREATE TABLE key_n (n numeric);
CREATE UNIQUE INDEX key_n_i ON key_n USING sift_col (n);
CREATE TABLE key_t (t text);
CREATE UNIQUE INDEX key_t_i ON key_t USING sift_col (t);
CREATE TABLE key_ab (a int, b text);
CREATE UNIQUE INDEX key_ab_i ON key_ab USING sift_col (a, b) NULLS NOT DISTINCT;
BEGIN;
INSERT INTO key_n VALUES (1.0);
INSERT INTO key_n VALUES (1.00);
ROLLBACK;
BEGIN;
INSERT INTO key_n VALUES (0.10), (1e30), (-5);
INSERT INTO key_n VALUES (0.1000);
ROLLBACK;
INSERT INTO key_n VALUES (0.10), (1e30), (-5);
INSERT INTO key_n VALUES (1e30);
INSERT INTO key_n VALUES (0.1000);
INSERT INTO key_n VALUES (-5.0);
BEGIN;
INSERT INTO key_t VALUES ('0123456789abcdef'), ('0123456789abcdefg'), (repeat('x', 5000)), ('');
INSERT INTO key_t VALUES ('0123456789abcdef');
ROLLBACK;
BEGIN;
INSERT INTO key_t VALUES ('0123456789abcdef'), ('0123456789abcdefg'), (repeat('x', 5000)), ('');
INSERT INTO key_t VALUES ('0123456789abcdefg');
ROLLBACK;
BEGIN;
INSERT INTO key_t VALUES ('0123456789abcdef'), ('0123456789abcdefg'), (repeat('x', 5000)), ('');
INSERT INTO key_t VALUES (repeat('x', 5000));
ROLLBACK;
BEGIN;
INSERT INTO key_t VALUES ('0123456789abcdef'), ('0123456789abcdefg'), (repeat('x', 5000)), ('');
INSERT INTO key_t VALUES ('');
ROLLBACK;
INSERT INTO key_t VALUES ('0123456789abcdef'), ('0123456789abcdefg'), (repeat('x', 5000)), ('');
INSERT INTO key_t VALUES ('0123456789abcdef');
INSERT INTO key_t VALUES (repeat('x', 5000));
INSERT INTO key_t VALUES (NULL), (NULL);
INSERT INTO key_ab VALUES (1, NULL);
INSERT INTO key_ab VALUES (1, NULL);
INSERT INTO key_ab VALUES (NULL, NULL), (NULL, 'x');
INSERT INTO key_ab VALUES (NULL, NULL);
SELECT (SELECT count(*) FROM key_n) AS numerics, (SELECT count(*) FROM key_t) AS texts, (SELECT count(*) FROM key_ab) AS pairs;

-- more keys than the registry holds: the stage goes to the partitions and the keys stay unique (before and after the flush)
CREATE TABLE big_t (id int, v int);
CREATE UNIQUE INDEX big_i ON big_t USING sift_col (id);
BEGIN;
INSERT INTO big_t SELECT g, 0 FROM generate_series(1, 150000) g;
SAVEPOINT a;
INSERT INTO big_t VALUES (1, 1);
ROLLBACK TO a;
SAVEPOINT b;
INSERT INTO big_t VALUES (75000, 1);
ROLLBACK TO b;
SAVEPOINT c;
INSERT INTO big_t VALUES (150000, 1);
ROLLBACK TO c;
INSERT INTO big_t VALUES (150001, 1);
SAVEPOINT d;
INSERT INTO big_t VALUES (150001, 2);
ROLLBACK TO d;
COMMIT;
SELECT count(*) AS rows, count(DISTINCT id) AS ids FROM big_t;
INSERT INTO big_t VALUES (2, 2);
INSERT INTO big_t VALUES (150002, 2);
INSERT INTO big_t SELECT g, 0 FROM generate_series(150000, 150010) g ON CONFLICT (id) DO NOTHING;
SELECT count(*) AS rows, count(DISTINCT id) AS ids FROM big_t;
BEGIN;
INSERT INTO big_t SELECT g, 0 FROM generate_series(200000, 360000) g;
ROLLBACK;
SELECT count(*) AS rows FROM big_t;
INSERT INTO big_t SELECT g, 0 FROM generate_series(200000, 200010) g;
SELECT count(*) AS rows FROM big_t;

\set VERBOSITY terse
-- a prepared transaction holds its keys, in the partitions (its stage was flushed before PREPARE): a writer of one waits for it, and is refused (commit) or goes in (rollback)
CREATE TABLE pc_t (id int, v text);
CREATE UNIQUE INDEX pc_i ON pc_t USING sift_col (id);
BEGIN;
INSERT INTO pc_t VALUES (1, 'prepared'), (2, 'prepared');
PREPARE TRANSACTION 'col_unique_shm_1';
SET lock_timeout = '100ms';
INSERT INTO pc_t VALUES (1, 'waits');
INSERT INTO pc_t VALUES (2, 'waits') ON CONFLICT (id) DO NOTHING;
INSERT INTO pc_t VALUES (3, 'does not wait');
RESET lock_timeout;
COMMIT PREPARED 'col_unique_shm_1';
INSERT INTO pc_t VALUES (1, 'duplicate');
INSERT INTO pc_t VALUES (2, 'upsert') ON CONFLICT (id) DO UPDATE SET v = 'upserted';
BEGIN;
SAVEPOINT s;
INSERT INTO pc_t VALUES (10, 'rolled back');
ROLLBACK TO s;
INSERT INTO pc_t VALUES (11, 'prepared');
PREPARE TRANSACTION 'col_unique_shm_2';
SET lock_timeout = '100ms';
INSERT INTO pc_t VALUES (11, 'waits');
INSERT INTO pc_t VALUES (10, 'does not wait');
RESET lock_timeout;
ROLLBACK PREPARED 'col_unique_shm_2';
INSERT INTO pc_t VALUES (11, 'goes in');
SELECT id, v FROM pc_t ORDER BY id;

\set VERBOSITY default
-- an index made with the polymorphic class keeps working (the class of the indexes of earlier releases)
CREATE TABLE old_t (id int, v text);
CREATE UNIQUE INDEX old_i ON old_t USING sift_col (id sift.col_ops);
INSERT INTO old_t VALUES (1, 'a'), (2, 'b');
INSERT INTO old_t VALUES (2, 'c');
SELECT oc.opcname FROM pg_index x JOIN pg_opclass oc ON oc.oid = x.indclass[0] WHERE x.indexrelid = 'old_i'::regclass;
SELECT id, v FROM old_t WHERE id = 2;

-- an index of format major 1 is made with the class that is the default now, and refuses UNIQUE as it did (the testing build creates format major 1 on request; the release build has the
-- second answer from format major 2, whose UNIQUE index is there to be made)
CREATE TABLE f1_t (id int, v text);
INSERT INTO f1_t SELECT g, 'v' || g FROM generate_series(1, 100) g;
CREATE TEMP TABLE f1_res (below_10 int, unique_refused boolean);
DO $$
DECLARE
	f1 boolean := current_setting('sift.test_create_format1', true) IS NOT NULL;
	n int;
	refused boolean := true;
BEGIN
	IF f1 THEN
		PERFORM set_config('sift.test_create_format1', 'on', false);
	END IF;
	CREATE INDEX f1_i ON f1_t USING sift_col (id);
	SELECT count(*) INTO n FROM f1_t WHERE id < 10;
	IF f1 THEN
		BEGIN
			CREATE UNIQUE INDEX f1_u ON f1_t USING sift_col (id);
			refused := false;
		EXCEPTION WHEN feature_not_supported THEN
			refused := true;
		END;
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
	INSERT INTO f1_res VALUES (n, refused);
END $$;
SELECT * FROM f1_res;
SELECT oc.opcname FROM pg_index x JOIN pg_opclass oc ON oc.oid = x.indclass[0] WHERE x.indexrelid = 'f1_i'::regclass;

-- a sift_col index of a sift_table is refused as before
CREATE TABLE tbl_t (id int, v text) USING sift_table;
CREATE UNIQUE INDEX tbl_i ON tbl_t USING sift_col (id);

-- every pair of types of an equality in a typed family has the two-argument comparison function of its btree family as FUNCTION 1 (what a row comparison and an ordering index ask for)
SELECT f.opfname, count(*) AS pairs, count(p.amproc) AS with_cmp, bool_and(pr.pronargs = 2) AS two_args
FROM pg_amop ao JOIN pg_opfamily f ON f.oid = ao.amopfamily JOIN pg_am a ON a.oid = f.opfmethod AND a.amname = 'sift_col'
	LEFT JOIN pg_amproc p ON p.amprocfamily = f.oid AND p.amprocnum = 1 AND p.amproclefttype = ao.amoplefttype AND p.amprocrighttype = ao.amoprighttype
	LEFT JOIN pg_proc pr ON pr.oid = p.amproc
WHERE ao.amopstrategy = 3 AND f.opfname <> 'col_ops' AND f.opfname <> 'col_jsonb_ops' AND f.opfname <> 'col_array_ops'
GROUP BY f.opfname ORDER BY f.opfname;

-- ON CONFLICT of many rows, some of keys that exist: each key is looked up in the registry and in the partitions that can hold it
CREATE TABLE oc_many (id int, v int DEFAULT 1);
CREATE UNIQUE INDEX oc_many_i ON oc_many USING sift_col (id);
INSERT INTO oc_many SELECT g FROM generate_series(1, 200) g;
INSERT INTO oc_many SELECT g FROM generate_series(101, 400) g ON CONFLICT (id) DO NOTHING;
INSERT INTO oc_many SELECT g FROM generate_series(1, 300) g ON CONFLICT (id) DO UPDATE SET v = oc_many.v + 1;
SELECT count(*) AS rows, count(DISTINCT id) AS keys, sum(v) AS total FROM oc_many;
SELECT v, count(*) FROM oc_many GROUP BY v ORDER BY v;

DROP TABLE ops_t, oc_t, dup_t, key_n, key_t, key_ab, big_t, pc_t, old_t, f1_t, tbl_t, oc_many;
DROP EXTENSION pg_sift;
