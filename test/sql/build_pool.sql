-- CREATE INDEX with a document that takes more of the inverted buffer's pool than its margin: the buffer is
-- spilled before the document is added, not once the pool is exhausted. The pool is 60,000 blocks of 64 kB
-- (a limit of the 32-bit addresses of the buffer), reached with a maintenance_work_mem of 4 GB or more;
-- sift.test_pool_blocks makes it 16 blocks, and each document 37,700 new terms of about 4.6 blocks.
CREATE EXTENSION pg_sift;
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '64MB';

CREATE TABLE pl (id serial, body text);
INSERT INTO pl (body)
SELECT 'common ' || (SELECT string_agg('q' || d || 'z' || j, ' ') FROM generate_series(1, 37700) j)
FROM generate_series(1, 8) d;

CREATE FUNCTION answers() RETURNS text AS $$
	SELECT (SELECT count(*) FROM pl WHERE body @@> 'common') || ' ' ||
	       (SELECT md5(string_agg(id::text, ',' ORDER BY id)) FROM pl WHERE body @@> 'q3z777 OR q8z37700 OR q1z1')
$$ LANGUAGE sql;
CREATE INDEX pl_ref ON pl USING sift (body);
CREATE TABLE ref AS SELECT answers() AS a;
DROP INDEX pl_ref;

SET sift.test_pool_blocks = 16;
CREATE INDEX pl_serial ON pl USING sift (body);
SELECT sift.verify('pl_serial', true);
SELECT answers() = a AS serial_same FROM ref;
DROP INDEX pl_serial;

SET max_parallel_maintenance_workers = 2;
ALTER TABLE pl SET (parallel_workers = 2);
CREATE INDEX pl_parallel ON pl USING sift (body);
SELECT sift.verify('pl_parallel', true);
SELECT answers() = a AS parallel_same FROM ref;

DROP TABLE pl, ref;
DROP FUNCTION answers();
DROP EXTENSION pg_sift;
