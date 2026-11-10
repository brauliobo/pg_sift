-- The fill of a scope index expands the member keys of a batch of fact rows by the scope keys the scope relation has for them, under one snapshot (col/scope_fill.c).
-- Both ways to know the pairs (the relation read once, the btree probed per member) give the entries of the join, for members in 0..6 keys, NULL and unknown members.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (member int, scope_key int, PRIMARY KEY (scope_key, member)) WITH (autovacuum_enabled = false);
-- member m (1..20000) is in (m % 7) keys, drawn from 1..40 by a fixed spread: members 7, 14, .. are in none, the others in 1..6
INSERT INTO tenant_scope
	SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, 20000) m, generate_series(0, 5) j WHERE j < m % 7 GROUP BY 1, 2;
CREATE TABLE facts AS
	SELECT i AS ordinal, CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN 100000 + i ELSE 1 + (i * 7919) % 20000 END::int8 AS fk FROM generate_series(0, 4999) i;

CREATE FUNCTION expand_md5(mode int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE format('SET sift.test_conn_fill = %s', mode);
	SELECT md5(string_agg(ordinal || ':' || key, ',' ORDER BY ordinal, key)) INTO r
		FROM sift._test_scope_expand('tenant_scope', 'tenant_scope_pkey', 'member', 'scope_key', (SELECT array_agg(fk ORDER BY ordinal) FROM facts));
	RETURN r;
END $$;

SELECT md5(string_agg(f.ordinal || ':' || s.scope_key, ',' ORDER BY f.ordinal, s.scope_key)) AS joined FROM facts f JOIN tenant_scope s ON s.member = f.fk \gset
SELECT expand_md5(1) = :'joined' AS key_set_is_the_join, expand_md5(2) = :'joined' AS probe_is_the_join, expand_md5(0) = :'joined' AS price_is_the_join;

-- keys per row of the expansion: 0..6, orphans and NULLs none
SELECT n, count(*) AS rows FROM (
	SELECT f.ordinal, count(e.key) AS n FROM facts f
		LEFT JOIN sift._test_scope_expand('tenant_scope', 'tenant_scope_pkey', 'member', 'scope_key', (SELECT array_agg(fk ORDER BY ordinal) FROM facts)) e USING (ordinal)
		GROUP BY 1) t GROUP BY 1 ORDER BY 1;

-- the pairs that fit half of maintenance_work_mem (about 58,000 pairs of 16 bytes at 1MB: more here) are read once, else probed: the same entries
SET maintenance_work_mem = '1MB';
SELECT expand_md5(0) = :'joined' AS small_memory_is_the_join;
RESET maintenance_work_mem;

-- a deleted pair is expanded by neither way
DELETE FROM tenant_scope WHERE member = 1;
SELECT expand_md5(1) = expand_md5(2) AS ways_agree_after_edit;
SELECT count(*) = 0 AS member_one_gone FROM sift._test_scope_expand('tenant_scope', 'tenant_scope_pkey', 'member', 'scope_key', ARRAY[1::int8]);

DROP FUNCTION expand_md5(int);
DROP TABLE facts;
DROP TABLE tenant_scope;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
