-- A mutable segment larger than maintenance_work_mem is promoted through a sort that spills to disk, in key order, and
-- the immutable segment answers like the mutable one did.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET maintenance_work_mem = '1MB';
SET enable_seqscan = off;
CREATE TABLE ps (id serial, body text) WITH (autovacuum_enabled = off);
CREATE INDEX ps_idx ON ps USING sift (body) WITH (mutable_limit = 1048576);
INSERT INTO ps (body)
SELECT string_agg('t' || ((g * 7 + j * 13) % 2003) || ' u' || (g % 31) || ' common', ' ') FROM generate_series(1, 6000) g, generate_series(1, 40) j GROUP BY g ORDER BY g;
DELETE FROM ps WHERE id % 10 = 0;
SELECT kind, ndocs FROM sift.segments('ps_idx') ORDER BY gen;
SELECT count(*) FROM ps WHERE body @@> 'common';
SELECT count(*) FROM ps WHERE body @@> 't5 AND u3';
SELECT count(*) FROM ps WHERE body @@> 't1000 OR t1999';
SELECT sift.promote('ps_idx');
SELECT kind, ndocs FROM sift.segments('ps_idx') ORDER BY gen;
SELECT count(*) FROM ps WHERE body @@> 'common';
SELECT count(*) FROM ps WHERE body @@> 't5 AND u3';
SELECT count(*) FROM ps WHERE body @@> 't1000 OR t1999';
SELECT sift.verify('ps_idx', true);
DROP TABLE ps;
DROP EXTENSION pg_sift;
