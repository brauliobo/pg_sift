-- An UPDATE that is not HOT and changes no column of a sift_col index (aminsert is told indexUnchanged) gives the index the new address of the row as a one-attribute record of its column delta
-- chain, without the comparison of the old and the new row. An index with a projection keeps the tombstone (its projections are read only from parts without a chain), and keeps serving them.
-- requires: build col
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pageinspect;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE SCHEMA unch;
SET search_path = unch, public;
\i test/common/sql/col/proj/base.sql

CREATE TABLE c (id bigint, tenant int, status text, amount bigint, other int, note text) WITH (autovacuum_enabled = false);
CREATE TABLE ch (LIKE c) WITH (autovacuum_enabled = false);
CREATE TABLE p (LIKE c) WITH (autovacuum_enabled = false);
CREATE INDEX c_other ON c (other);
CREATE INDEX p_other ON p (other);
CREATE INDEX c_idx ON c USING sift_col (id, tenant, status, amount) WITH (key_order = 'id');
CREATE INDEX p_idx ON p USING sift_col (id, tenant, status, amount) WITH (key_order = 'id');
INSERT INTO c SELECT i, i % 5, (ARRAY['new', 'open', 'closed'])[1 + i % 3], i % 50, i % 7, 'n' || i FROM generate_series(1, 20000) i;
INSERT INTO ch SELECT * FROM c;
INSERT INTO p SELECT * FROM c;
SELECT sift.create_projection('p_idx', 'p_agg', 'GROUP BY tenant, status MEASURES count(*), sum(amount)');
VACUUM (FREEZE) c, p;

CREATE FUNCTION delta_pages(idx regclass) RETURNS int LANGUAGE sql AS $$
	SELECT count(*)::int FROM generate_series(0, pg_relation_size(idx) / 8192 - 1) n
	WHERE get_byte(get_raw_page(idx::text, n::int), 8176) + 256 * get_byte(get_raw_page(idx::text, n::int), 8177) = 42 $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE sql AS
	$$ SELECT p_rows(q, true) = p_rows(regexp_replace(q, 'FROM (c|p)\y', 'FROM ch'), false) $$;

SELECT p_check('no record yet', delta_pages('c_idx') = 0 AND delta_pages('p_idx') = 0);

-- the B-tree index on other changes: the row moves, no column of the sift_col index changes
UPDATE c SET other = other + 1 WHERE id % 10 = 0;
UPDATE p SET other = other + 1 WHERE id % 10 = 0;
UPDATE ch SET other = other + 1 WHERE id % 10 = 0;
SELECT delta_pages('c_idx') AS pages_one \gset
SELECT p_check('the new addresses are records of the chain', :pages_one > 0);
SELECT p_check('rows equal the heap', same('SELECT id, tenant, status, amount, other FROM c WHERE id % 10 = 0 OR tenant = 3'));
SELECT p_check('aggregate equals the heap', same('SELECT tenant, status, count(*), sum(amount) FROM c GROUP BY tenant, status'));
SELECT p_check('the projected index writes no record', delta_pages('p_idx') = 0);
SELECT p_check('the projection answers as the heap', same('SELECT tenant, status, count(*), sum(amount) FROM p GROUP BY tenant, status'));

-- a column of the index changes: no hint, the rows are compared
UPDATE c SET amount = amount + 1 WHERE id % 10 = 3;
UPDATE ch SET amount = amount + 1 WHERE id % 10 = 3;
SELECT p_check('aggregate follows the value', same('SELECT tenant, status, count(*), sum(amount) FROM c GROUP BY tenant, status'));
SELECT p_check('every row equals the heap', same('SELECT id, tenant, status, amount, other FROM c'));

VACUUM (FREEZE) c, p;
SELECT p_check('the projection is served after vacuum', p_uses('SELECT tenant, status, count(*), sum(amount) FROM p GROUP BY tenant, status', 'p_agg'));
SELECT p_check('aggregate after vacuum', same('SELECT tenant, status, count(*), sum(amount) FROM c GROUP BY tenant, status'));
SELECT p_check('every row after vacuum', same('SELECT id, tenant, status, amount, other FROM c'));
SELECT p_check('sift.check is clean', count(*) = 0) FROM (SELECT * FROM sift.check('c_idx'::regclass) UNION ALL SELECT * FROM sift.check('p_idx'::regclass)) x;

DROP SCHEMA unch CASCADE;
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
