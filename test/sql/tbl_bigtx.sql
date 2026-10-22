-- One transaction that writes more row-format pages than the directory has entries for. A running transaction's rows cannot be final, so
-- a write buffer cannot become a frozen column group while it runs; once the directory is half full a write buffer that one command wrote
-- becomes a group that carries its writer (xid and command) and is invisible to everybody else until the writer commits, like the rows it
-- holds. The limit is lowered here (a testing setting) to reach it with a few dozen write buffers of 2 MB.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.table_max_segments = 16;
SET sift.table_write_buffer_limit = 2048;
SET max_parallel_workers_per_gather = 0;

-- a heap twin takes the same statements: the answers must be the same
CREATE FUNCTION digest(t regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) || '':'' || coalesce(md5(string_agg(id || v, '','' ORDER BY id, v)), '''') FROM %s', t) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION same(a regclass, b regclass) RETURNS bool LANGUAGE sql AS $$ SELECT digest(a) = digest(b) $$;
CREATE FUNCTION nsegs(t regclass, OUT total int, OUT columns int, OUT sealed int) LANGUAGE sql AS
$$ SELECT count(*)::int, (count(*) FILTER (WHERE kind = 'columns'))::int, (count(*) FILTER (WHERE kind = 'sealed buffer'))::int FROM sift.table_segments(t) $$;
-- rows of about a kilobyte: 40000 of them are about 20 write buffers
CREATE FUNCTION load(t regclass, lo int, hi int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('INSERT INTO %s SELECT g, repeat(''x'', 1000) || g FROM generate_series(%s, %s) g', t, lo, hi);
END $$;
-- the statements of the next section, one command each
CREATE FUNCTION rewrite(t regclass) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM load(t, 1, 40000);
	EXECUTE format('UPDATE %s SET v = v || ''u'' WHERE id %% 3 = 0', t);
	EXECUTE format('DELETE FROM %s WHERE id %% 5 = 0', t);
	EXECUTE format('INSERT INTO %1$s SELECT id + 100000, v FROM %1$s WHERE id %% 2 = 0', t);
END $$;

CREATE TABLE ins_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE ins_h (id int, v text) WITH (autovacuum_enabled = false);

-- INSERT ... SELECT
BEGIN;
INSERT INTO ins_t SELECT g, repeat('x', 1000) || g FROM generate_series(1, 40000) g;
SELECT count(*), sum(id), count(*) FILTER (WHERE v <> repeat('x', 1000) || id) AS wrong FROM ins_t;
SELECT total <= 14 AS within_the_limit, columns > 0 AS promoted_while_running, sift._test_tbl_tagged('ins_t') > 0 AS carries_its_writer FROM nsegs('ins_t');
COMMIT;
SELECT sift._test_tbl_tagged('ins_t') > 0 AS still_tagged;
INSERT INTO ins_h SELECT g, repeat('x', 1000) || g FROM generate_series(1, 40000) g;
SELECT same('ins_t', 'ins_h') AS the_twin_agrees;
SELECT count(*), sum(id), count(*) FILTER (WHERE v <> repeat('x', 1000) || id) AS wrong FROM ins_t;

-- the rows are final: VACUUM freezes the groups and the table answers the same
VACUUM ins_t;
SELECT same('ins_t', 'ins_h') AS the_twin_agrees_after_vacuum;
SELECT sealed = 0 AS nothing_waits, sift._test_tbl_tagged('ins_t') = 0 AS frozen FROM nsegs('ins_t');
-- the index scans see the same rows
CREATE INDEX ins_t_id ON ins_t (id);
SET enable_seqscan = off;
SELECT count(*), sum(id) FROM ins_t WHERE id BETWEEN 100 AND 30000;
RESET enable_seqscan;
SELECT count(*), sum(id) FROM ins_h WHERE id BETWEEN 100 AND 30000;

-- COPY
CREATE TABLE copy_t (id int, v text, pad text DEFAULT repeat('p', 1000)) USING sift_table WITH (autovacuum_enabled = false);
BEGIN;
COPY copy_t (id, v) FROM PROGRAM 'seq 1 40000 | awk ''{ print $1 "\tv" $1 }''';
SELECT count(*), sum(id), count(*) FILTER (WHERE v <> 'v' || id OR pad <> repeat('p', 1000)) AS wrong FROM copy_t;
SELECT total <= 14 AS within_the_limit, columns > 0 AS promoted_while_running, sift._test_tbl_tagged('copy_t') > 0 AS carries_its_writer FROM nsegs('copy_t');
COMMIT;
SELECT count(*), sum(id), count(*) FILTER (WHERE v <> 'v' || id OR pad <> repeat('p', 1000)) AS wrong FROM copy_t;
VACUUM copy_t;
SELECT sift._test_tbl_tagged('copy_t') = 0 AS frozen;
SELECT count(*), sum(id), count(*) FILTER (WHERE v <> 'v' || id OR pad <> repeat('p', 1000)) AS wrong FROM copy_t;

-- ROLLBACK: nothing is left, and the space comes back after VACUUM
CREATE TABLE rb_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE INDEX rb_t_id ON rb_t (id);
BEGIN;
INSERT INTO rb_t SELECT g, repeat('x', 1000) || g FROM generate_series(1, 40000) g;
SELECT count(*) FROM rb_t;
ROLLBACK;
SELECT count(*) FROM rb_t;
SELECT sift._test_tbl_tagged('rb_t') > 0 AS groups_wait_for_vacuum;
SELECT relation_blocks AS rolled_back_blocks FROM sift.table_space('rb_t') \gset
VACUUM rb_t;
VACUUM rb_t;
SELECT count(*) FROM rb_t;
SELECT sift._test_tbl_tagged('rb_t') = 0 AS groups_gone, relation_blocks < :rolled_back_blocks / 4 AS space_returned
	FROM nsegs('rb_t'), sift.table_space('rb_t');
-- the table takes a new load as if it were new
INSERT INTO rb_t SELECT g, repeat('x', 1000) || g FROM generate_series(1, 40000) g;
SELECT count(*), sum(id) FROM rb_t;
SET enable_seqscan = off;
SELECT count(*), sum(id) FROM rb_t WHERE id > 20000;
RESET enable_seqscan;

-- a savepoint rolled back in the middle: its rows go, the ones around it stay
CREATE TABLE sp_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE sp_h (id int, v text) WITH (autovacuum_enabled = false);
BEGIN;
SELECT load('sp_t', 1, 15000);
SAVEPOINT s;
SELECT load('sp_t', 100001, 140000);
SELECT count(*) FROM sp_t;
SELECT sift._test_tbl_tagged('sp_t') > 0 AS the_subtransaction_has_groups;
ROLLBACK TO s;
SELECT count(*) FROM sp_t;
SELECT load('sp_t', 15001, 40000);
COMMIT;
SELECT load('sp_h', 1, 40000);
SELECT same('sp_t', 'sp_h') AS the_twin_agrees;
-- ... and one released into its parent stays with it
BEGIN;
SAVEPOINT s;
SELECT load('sp_t', 200001, 240000);
RELEASE s;
COMMIT;
SELECT count(*) FROM sp_t WHERE id > 200000;
VACUUM sp_t;
SELECT count(*) FROM sp_t;

-- UPDATE and DELETE of the rows of the transaction itself
CREATE TABLE ud_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE ud_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX ud_t_id ON ud_t (id);
CREATE INDEX ud_h_id ON ud_h (id);
BEGIN;
SELECT rewrite('ud_t');
SELECT count(*), count(*) FILTER (WHERE v LIKE '%u') AS updated, count(*) FILTER (WHERE id % 5 = 0) AS deleted FROM ud_t;
SELECT sift._test_tbl_tagged('ud_t') > 0 AS carries_its_writer;
COMMIT;
SELECT rewrite('ud_h');
SELECT same('ud_t', 'ud_h') AS the_twin_agrees;
-- a statement that reads the table it writes: an index scan finds the rows it just wrote, which the command that wrote them does not see.
-- The scan starts at the copies (id > 100000, 16000 rows: as many sealed write buffers as all 48000 rows leave) and its range reaches the new keys
BEGIN;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
INSERT INTO ud_t SELECT id + 1000000, v FROM ud_t WHERE id > 100000;
INSERT INTO ud_h SELECT id + 1000000, v FROM ud_h WHERE id > 100000;
COMMIT;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT same('ud_t', 'ud_h') AS the_twin_agrees_after_self_insert;
-- index probes in an order that moves between more superblocks than the table keeps fetch scans for (the versions of an updated row sit in other ones)
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT (SELECT (count(*), sum(t.id), sum(length(t.v)), sum(hashtext(t.v))) FROM generate_series(1, 300) g JOIN ud_t t ON t.id = (g * 7919) % 140000 + 1) IS NOT DISTINCT FROM
	   (SELECT (count(*), sum(t.id), sum(length(t.v)), sum(hashtext(t.v))) FROM generate_series(1, 300) g JOIN ud_h t ON t.id = (g * 7919) % 140000 + 1) AS probes_agree;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_hashjoin;
RESET enable_mergejoin;
VACUUM ud_t;
VACUUM ud_h;
SELECT same('ud_t', 'ud_h') AS the_twin_agrees_after_vacuum;

-- parallel plans, before and after the groups are frozen
CREATE TABLE par_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
BEGIN;
SELECT load('par_t', 1, 40000);
COMMIT;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET sift.enable_agg = off;
EXPLAIN (COSTS OFF) SELECT count(*), sum(id) FROM par_t;
SELECT count(*), sum(id) FROM par_t;
VACUUM par_t;
EXPLAIN (COSTS OFF) SELECT count(*), sum(id) FROM par_t;
SELECT count(*), sum(id) FROM par_t;

RESET max_parallel_workers_per_gather;
DROP TABLE ins_t, ins_h, copy_t, rb_t, sp_t, sp_h, ud_t, ud_h, par_t;
DROP FUNCTION digest(regclass), same(regclass, regclass), nsegs(regclass), load(regclass, int, int), rewrite(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
