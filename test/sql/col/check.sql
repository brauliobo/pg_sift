-- sift.check: one row per problem, no row when the indexes are sound, damage reported and never raised; the privilege of the verifiers;
-- the raising forms name what to do; a failure inside a scan names the index
CREATE EXTENSION pg_sift_test CASCADE;
-- Format 1: the pages it damages and checks are Format 1 segment pages.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
CREATE TABLE ck (id int PRIMARY KEY, a int, b text);
INSERT INTO ck SELECT i, i % 10, 'v' || i % 50 FROM generate_series(1, 20000) i;
CREATE INDEX ck_col ON ck USING sift_col (a, b);
CREATE INDEX ck_bm ON ck USING sift (b);
INSERT INTO ck SELECT i, i % 10, 'w' || i % 50 FROM generate_series(20001, 40000) i;
SELECT sift.col_promote('ck_col') > 0 AS promoted;
SELECT gen, kind FROM sift.col_segments('ck_col') ORDER BY gen;
CREATE FUNCTION data_block(idx regclass, g int8) RETURNS int8 LANGUAGE sql AS $$
	SELECT min(b) FROM generate_series(0, pg_relation_size(idx) / 8192 - 1) b WHERE (sift._test_page(idx, b)).type = 4 AND (sift._test_page(idx, b)).gen = g
$$;

CREATE FUNCTION raised(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	h text;
	c text;
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS h = PG_EXCEPTION_HINT, c = PG_EXCEPTION_CONTEXT;
	RETURN SQLSTATE || ': ' || SQLERRM || coalesce(' | ' || nullif(h, ''), '') || ' | ' || split_part(c, E'\n', 1);
END $$;

-- sound: no row, whatever the target and the depth
SELECT count(*) AS all_sound FROM sift.check();
SELECT count(*) AS col_sound FROM sift.check('ck_col', true);
SELECT count(*) AS bm25_sound FROM sift.check('ck_bm', true);

-- damaged: the problem is a row of the segment, no error; the segments that are sound are not named, and the ones after a damaged one are checked
SELECT sift._test_poke('ck_col', data_block('ck_col', 4), 8176, 9, 2);
SELECT index, segment, problem, regexp_replace(detail, 'Block [0-9]+', 'Block N') AS detail FROM sift.check('ck_col');
SELECT sift._test_poke('ck_col', data_block('ck_col', 1), 8176, 9, 2);
SELECT index, segment, problem FROM sift.check('ck_col', true);
SELECT index, segment FROM sift.check() ORDER BY 1, 2;
SELECT count(*) AS capped FROM sift.check('ck_col', false, 1);
SELECT raised($$ SELECT sift.col_verify('ck_col') $$);
SELECT raised($$ SELECT sift.verify('ck_col') $$);
SELECT raised($$ SELECT count(*) FROM sift.check('ck_col') $$);
REINDEX INDEX ck_col;

SELECT sift._test_poke('ck_bm', 3, 8184, 77, 8);
SELECT index, segment, problem, regexp_replace(detail, 'Block [0-9]+', 'Block N') AS detail FROM sift.check('ck_bm');
SELECT raised($$ SELECT sift.verify('ck_bm') $$);
REINDEX INDEX ck_bm;
SELECT count(*) AS repaired FROM sift.check();

-- a sift_table
CREATE TABLE ct (id int, a int, b text) USING sift_table;
INSERT INTO ct SELECT i, i % 10, 'v' || i % 50 FROM generate_series(1, 20000) i;
VACUUM ct;
SELECT sift.table_verify('ct');
SELECT count(*) AS table_sound FROM sift.check('ct', true);

-- privilege: the owner or MAINTAIN, as for VACUUM
CREATE ROLE ck_user;
GRANT SELECT ON ck, ct TO ck_user;
SET ROLE ck_user;
SELECT raised($$ SELECT sift.col_verify('ck_col') $$);
SELECT raised($$ SELECT sift.verify('ck_bm') $$);
SELECT raised($$ SELECT sift.verify('ck_col') $$);
SELECT raised($$ SELECT * FROM sift.check('ck_col') $$);
SELECT raised($$ SELECT * FROM sift.check('ck_bm') $$);
SELECT raised($$ SELECT sift.table_verify('ct') $$);
SELECT raised($$ SELECT * FROM sift.check('ct') $$);
SELECT count(*) AS walked_by_user FROM sift.check();
RESET ROLE;
GRANT MAINTAIN ON ck, ct TO ck_user;
SET ROLE ck_user;
SELECT sift.col_verify('ck_col'), sift.verify('ck_bm');
SELECT sift.table_verify('ct');
SELECT count(*) AS walked_by_maintainer FROM sift.check();
RESET ROLE;

-- an error below a scan that does not name the index carries it as context: the first damage (one byte of a page of the index) whose error does not
-- name the index, for each way to read it, and the first line of its context
CREATE FUNCTION unnamed_context(idx regclass, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	c text;
BEGIN
	FOR blk IN 2 .. pg_relation_size(idx) / 8192 - 1 LOOP
		FOR off IN 20 .. 3000 BY 37 LOOP
			EXECUTE format('REINDEX INDEX %s', idx);
			PERFORM sift._test_poke(idx, blk, off, 255, 1);
			BEGIN
				EXECUTE q;
			EXCEPTION WHEN OTHERS THEN
				GET STACKED DIAGNOSTICS c = PG_EXCEPTION_CONTEXT;
				IF SQLERRM NOT LIKE '%' || idx::text || '%' THEN
					EXECUTE format('REINDEX INDEX %s', idx);
					RETURN split_part(c, E'\n', 1);
				END IF;
			END;
		END LOOP;
	END LOOP;
	EXECUTE format('REINDEX INDEX %s', idx);
	RETURN NULL;
END $$;
SET client_min_messages = warning;
SELECT unnamed_context('ck_col', $$ SELECT a, count(*) FROM ck WHERE b = 'v3' GROUP BY a $$) AS aggregate_scan;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET sift.enable_agg = off;
SELECT unnamed_context('ck_col', $$ SELECT id FROM ck WHERE a = 3 AND id < 300 $$) AS bitmap_scan;
SELECT unnamed_context('ck_bm', $$ SELECT count(*) FROM ck WHERE b @@> 'v3' $$) AS bm25_scan;
RESET sift.enable_agg;
RESET enable_indexscan;
RESET enable_seqscan;

-- a damaged page of a sift_table: every way to read the table names it, once (the page holds the first row at a misaligned offset)
CREATE TABLE cw (id int PRIMARY KEY, a int) USING sift_table;
INSERT INTO cw SELECT i, i FROM generate_series(1, 100) i;
SELECT sift._test_poke('cw', max(b), 40, 1, 2) FROM generate_series(0, pg_relation_size('cw') / 8192 - 1) b WHERE (sift._test_page('cw', b)).type = 7;
CREATE FUNCTION table_error(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	c text;
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS c = PG_EXCEPTION_CONTEXT;
	RETURN SQLERRM || ' | ' || split_part(c, E'\n', 1) || ' | lines: ' || regexp_count(c, 'sift table');
END $$;
SELECT table_error($$ SELECT count(*) FROM cw $$) AS seq_scan;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT table_error($$ SELECT a FROM cw WHERE id = 1 $$) AS index_scan;
RESET enable_bitmapscan;
SET enable_indexscan = off;
SELECT table_error($$ SELECT count(*) FROM cw WHERE id < 50 $$) AS bitmap_scan;
RESET enable_indexscan;
RESET enable_seqscan;
SELECT table_error($$ ANALYZE cw $$) AS analyze_scan;
SELECT table_error($$ SELECT count(*) FROM cw TABLESAMPLE SYSTEM (100) $$) AS sample_scan;
DROP TABLE cw;
DROP FUNCTION table_error(text);
RESET client_min_messages;

REVOKE ALL ON ck, ct FROM ck_user;
DROP ROLE ck_user;
DROP TABLE ck, ct;
DROP FUNCTION raised(text), unnamed_context(regclass, text), data_block(regclass, int8);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
