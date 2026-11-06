-- The write buffer of a BM25 index in format 6 (SIFT_WBUF_FORMAT_WIDE) has a bloom of 8,192 bits for each full page, filled when the page is full, where format 0 has 256 bits for
-- the page, which a page of 120 terms a document fills: a term in no document of the buffer then reads every page. The rows are those of the buffer of format 0, of a buffer being appended to
-- (its pages have the small bloom), of the segments after the promotion, and of the heap; a buffer keeps the format it was opened with, and VACUUM, verify and deletes read both.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;

-- 120 terms in a document, 'common' in every document, the others spread so that a page of 6 documents holds 700 different ones
CREATE FUNCTION ww_doc(i int) RETURNS text LANGUAGE sql IMMUTABLE AS
$$ SELECT 'common ' || (SELECT string_agg('t' || ((i * 7 + k * 13) % 4001), ' ') FROM generate_series(1, 120) k) || ' ' || (ARRAY['red', 'green', 'blue'])[1 + i % 3] $$;

CREATE TABLE ww (id int, body text) WITH (autovacuum_enabled = false);
CREATE INDEX ww_body ON ww USING sift (body);
INSERT INTO ww SELECT i, ww_doc(i) FROM generate_series(1, 1200) i;

SET sift.test_wbuf_narrow = on;
CREATE TABLE wn (id int, body text) WITH (autovacuum_enabled = false);
CREATE INDEX wn_body ON wn USING sift (body);
INSERT INTO wn SELECT i, ww_doc(i) FROM generate_series(1, 1200) i;
RESET sift.test_wbuf_narrow;

-- the format of the buffer each opened with
SELECT 'ww' AS tbl, format, ndocs = 1200 AS all_in_the_buffer FROM sift._test_wbuf_reads('ww_body') UNION ALL SELECT 'wn', format, ndocs = 1200 FROM sift._test_wbuf_reads('wn_body') ORDER BY 1;

-- a term in no document: the wide bloom reads the pages being appended to and a few false ones, the small one nearly all
CREATE FUNCTION ww_walk(tbl text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE w0 bigint; w1 bigint;
BEGIN
	EXECUTE format('SELECT walked FROM sift._test_wbuf_reads(%L)', tbl || '_body') INTO w0;
	EXECUTE format('SELECT count(*) FROM %I WHERE body @@> %L', tbl, q);
	EXECUTE format('SELECT walked FROM sift._test_wbuf_reads(%L)', tbl || '_body') INTO w1;
	RETURN w1 - w0;
END $$;
SELECT ww_walk('ww', 'qqabsent') * 5 < ndocs AS wide_skips_pages, ww_walk('wn', 'qqabsent') * 10 > ndocs * 9 AS small_does_not, ww_walk('ww', 'common') = ndocs AS common_reads_all
FROM sift._test_wbuf_reads('ww_body');

-- the same rows from the buffer of either format, the segments after promotion and the heap
CREATE FUNCTION ww_rows(tbl text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '' '' || coalesce(sum(id), 0) FROM %I WHERE body @@> %L', tbl, q) INTO r;
	RETURN r;
END $$;
CREATE TABLE ww_q (n int, q text);
INSERT INTO ww_q VALUES (1, 't7'), (2, 'red'), (3, 'red blue'), (4, 'red OR green'), (5, 'common NOT red'), (6, 't7 t20 t33'), (7, 'qqabsent'), (8, 't1500 OR t3999'), (9, '"t7 t20"'), (10, 't1*');
SELECT n, q, ww_rows('ww', q) AS wide, ww_rows('ww', q) = ww_rows('wn', q) AS same_as_narrow FROM ww_q ORDER BY n;

-- appended to, updated, deleted
INSERT INTO ww SELECT i, ww_doc(i) FROM generate_series(1201, 1500) i;
INSERT INTO wn SELECT i, ww_doc(i) FROM generate_series(1201, 1500) i;
UPDATE ww SET body = ww_doc(id + 1) WHERE id % 7 = 0;
UPDATE wn SET body = ww_doc(id + 1) WHERE id % 7 = 0;
DELETE FROM ww WHERE id % 11 = 0;
DELETE FROM wn WHERE id % 11 = 0;
SELECT n, ww_rows('ww', q) = ww_rows('wn', q) AS same_as_narrow, ww_rows('ww', q) AS rows FROM ww_q ORDER BY n;
VACUUM ww, wn;
SELECT sift.verify('ww_body'), sift.verify('wn_body');
SELECT n, ww_rows('ww', q) = ww_rows('wn', q) AS same_as_narrow FROM ww_q ORDER BY n;

-- promoted: the narrow buffer's segments and a wide buffer opened after them
SELECT sift.promote('wn_body'), sift.promote('ww_body');
INSERT INTO ww SELECT i, ww_doc(i) FROM generate_series(2001, 2400) i;
INSERT INTO wn SELECT i, ww_doc(i) FROM generate_series(2001, 2400) i;
SELECT format FROM sift._test_wbuf_reads('wn_body');
SELECT n, ww_rows('ww', q) = ww_rows('wn', q) AS same_as_narrow FROM ww_q ORDER BY n;
SELECT sift.verify('ww_body'), sift.verify('wn_body');

DROP FUNCTION ww_doc(int), ww_walk(text, text), ww_rows(text, text);
DROP TABLE ww, wn, ww_q;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
