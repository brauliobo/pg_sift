-- The open write buffer of a BM25 index is walked by every query (its pages pass a 256-bit bloom that a page of 900 terms fills), 250 cycles a document, and costs 55,000 cycles a
-- document to promote. The documents the queries walked are noted per generation of the buffer; once they have paid half of the promotion (a buffer of 2,000 documents or more)
-- the worker seals and promotes it. A buffer that is only written has no walks and never promotes before its limit. The worker is off here: the test asks the rule.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE wr (id int, body text) WITH (autovacuum_enabled = false);
INSERT INTO wr SELECT i, 'alpha ' || (ARRAY['beta', 'gamma', 'delta'])[1 + i % 3] || ' ' || i FROM generate_series(1, 200) i;
CREATE INDEX wr_body ON wr USING sift (body);
INSERT INTO wr SELECT i, 'alpha ' || (ARRAY['beta', 'gamma', 'delta'])[1 + i % 3] || ' ' || i FROM generate_series(201, 3200) i;

-- written, never read: 3,000 documents in the buffer, nothing walked, nothing due
SELECT * FROM sift._test_wbuf_reads('wr_body');

-- every query walks all 3,000 documents ('alpha' is in each); half of 55,000 x 3,000 cycles is 330,000 walks of 250 cycles: 110 queries
CREATE FUNCTION wr_read(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		PERFORM count(*) FROM wr WHERE body @@> 'alpha';
	END LOOP;
END $$;
SELECT wr_read(50);
SELECT walked = 50 * ndocs AS walked_50_full_buffers, ndocs, due FROM sift._test_wbuf_reads('wr_body');
SELECT wr_read(59);
SELECT walked = 109 * ndocs AS walked_109_full_buffers, due FROM sift._test_wbuf_reads('wr_body');
SELECT wr_read(2);
SELECT walked = 111 * ndocs AS walked_111_full_buffers, due FROM sift._test_wbuf_reads('wr_body');

-- the same rows with the buffer walked or promoted
SELECT count(*) FROM wr WHERE body @@> 'alpha delta';
SELECT sift.promote('wr_body');
SELECT * FROM sift._test_wbuf_reads('wr_body');
SELECT count(*) FROM wr WHERE body @@> 'alpha delta';

-- a buffer under 2,000 documents is never due, however much is read
INSERT INTO wr SELECT i, 'alpha beta ' || i FROM generate_series(4001, 5000) i;
SELECT wr_read(400);
SELECT ndocs, walked > 0 AS walked, due FROM sift._test_wbuf_reads('wr_body');

DROP FUNCTION wr_read(int);
DROP TABLE wr;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
