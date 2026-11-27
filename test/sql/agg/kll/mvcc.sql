-- sift.approx_percentile answers from the sketches of the parts only for rows the statement sees: a part of a transaction that rolled back, a part this transaction wrote and the rows
-- it staged are answered exactly. A part carries a sketch from 32768 rows on, when the sketch fits the auxiliary budget of its data (the md5 column keeps the data incompressible).
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
CREATE TABLE km (id int8, v float8, pad text) WITH (autovacuum_enabled = false);
CREATE INDEX km_col ON km USING sift_col (id, v, pad) WITH (sketches = 'kll(v)');
INSERT INTO km SELECT i, i, md5(i::text) FROM generate_series(1, 33000) i;
SELECT sift.approx_percentile('km', 'v', 1) AS committed;

-- a serializable reader of the sketches conflicts with a writer of the table
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT sift.approx_percentile('km', 'v', 1) AS serializable;
SELECT count(*) > 0 AS predicate_locked FROM pg_locks WHERE locktype = 'relation' AND mode = 'SIReadLock' AND relation = 'km'::regclass AND pid = pg_backend_pid();
COMMIT;

-- a part of this transaction, then of a transaction that rolled back
BEGIN;
INSERT INTO km SELECT i, 1e9, md5(i::text) FROM generate_series(1, 33000) i;
SELECT sift._test_f2_stage_flush('km_col');
SELECT sift.approx_percentile('km', 'v', 1) AS own_part;
ROLLBACK;
SELECT count(*) AS parts FROM sift.parts('km_col') WHERE kind = 'immutable';
SELECT sift.approx_percentile('km', 'v', 1) AS after_rollback, sift.approx_quantiles('km', 'v', ARRAY[0.5, 1]) AS quantiles;

-- the rows this transaction staged
BEGIN;
INSERT INTO km SELECT i, 2e9, md5(i::text) FROM generate_series(1, 50) i;
SELECT sift.approx_percentile('km', 'v', 1) AS own_staged;
ROLLBACK;

DROP TABLE km;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
