-- sift.facets() on a table with row-level security: the counts are those of the GROUP BY of the same statement of the same role, so a row a policy hides counts in no value. Permissive and
-- restrictive policies, a policy with a sub-select, FORCE ROW LEVEL SECURITY, a heap and a sift_table, rows updated and deleted (HOT chains, other versions), a role that bypasses the policies.
-- Every comparison prints the rows of the two answers that differ: none.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
CREATE ROLE fr_alice;
CREATE ROLE fr_bob;
CREATE ROLE fr_admin BYPASSRLS;
CREATE SCHEMA fr;
GRANT USAGE ON SCHEMA fr TO fr_alice, fr_bob, fr_admin;
GRANT USAGE ON SCHEMA sift TO fr_alice, fr_bob, fr_admin;
SET search_path = fr, public;

CREATE TABLE fr_team (member text, team text);
INSERT INTO fr_team VALUES ('fr_alice', 'red'), ('fr_bob', 'blue'), ('fr_alice', 'blue');

CREATE TABLE fr_doc (id int PRIMARY KEY, owner text NOT NULL, team text NOT NULL, kind text, memo text NOT NULL, secret boolean NOT NULL);
INSERT INTO fr_doc
SELECT i, CASE i % 3 WHEN 0 THEN 'fr_alice' WHEN 1 THEN 'fr_bob' ELSE 'carol' END, CASE i % 2 WHEN 0 THEN 'red' ELSE 'blue' END,
	CASE WHEN i % 11 = 0 THEN NULL ELSE 'k' || i % 5 END, 'cable wire ' || CASE WHEN i % 4 = 0 THEN 'copper' ELSE 'steel' END || ' ' || i, i % 10 = 0
FROM generate_series(1, 6000) i;
CREATE TABLE fr_tbl (LIKE fr_doc INCLUDING ALL) USING sift_table;
INSERT INTO fr_tbl SELECT * FROM fr_doc;
CREATE INDEX fr_doc_memo ON fr_doc USING sift (memo);
CREATE INDEX fr_doc_kind ON fr_doc USING sift (kind) WITH (analyzer = keyword);
CREATE INDEX fr_tbl_memo ON fr_tbl USING sift (memo);
CREATE INDEX fr_tbl_kind ON fr_tbl USING sift (kind) WITH (analyzer = keyword);
GRANT SELECT ON fr_doc, fr_tbl, fr_team TO fr_alice, fr_bob, fr_admin;

CREATE FUNCTION fr_diff(tbl regclass, idx_memo regclass, idx_kind regclass, q text) RETURNS TABLE (facet_rows bigint, only_facets bigint, only_group_by bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE fr_f ON COMMIT DROP AS SELECT value::text AS v, count::bigint AS n FROM sift.facets(%L, %L, %L, 100, true)', idx_memo, idx_kind, q);
	EXECUTE format('CREATE TEMP TABLE fr_g ON COMMIT DROP AS SELECT kind AS v, count(*)::bigint AS n FROM %s WHERE memo @@> %L GROUP BY kind', tbl, q);
	RETURN QUERY SELECT (SELECT count(*) FROM fr_f), (SELECT count(*) FROM (TABLE fr_f EXCEPT ALL TABLE fr_g) a), (SELECT count(*) FROM (TABLE fr_g EXCEPT ALL TABLE fr_f) b);
END $$;
GRANT EXECUTE ON FUNCTION fr_diff(regclass, regclass, regclass, text) TO fr_alice, fr_bob, fr_admin;

CREATE FUNCTION fr_check(who text, tbl regclass, idx_memo regclass, idx_kind regclass, q text) RETURNS TABLE (role text, tbl_name text, query text, facet_rows bigint, only_facets bigint, only_group_by bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('SET LOCAL ROLE %I', who);
	RETURN QUERY SELECT who, tbl::text, q, d.* FROM fr_diff(tbl, idx_memo, idx_kind, q) d;
END $$;

-- no policy yet: every row counts
BEGIN;
SELECT * FROM fr_check('fr_alice', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable');
COMMIT;

ALTER TABLE fr_doc ENABLE ROW LEVEL SECURITY;
ALTER TABLE fr_tbl ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON fr_doc USING (owner = current_user);
CREATE POLICY own ON fr_tbl USING (owner = current_user);

-- the owner policy: each role sees its rows; the facets and the GROUP BY agree
BEGIN; SELECT * FROM fr_check('fr_alice', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable'); COMMIT;
BEGIN; SELECT * FROM fr_check('fr_bob', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'copper'); COMMIT;
BEGIN; SELECT * FROM fr_check('fr_alice', 'fr_tbl', 'fr_tbl_memo', 'fr_tbl_kind', 'cable'); COMMIT;
BEGIN; SELECT * FROM fr_check('fr_bob', 'fr_tbl', 'fr_tbl_memo', 'fr_tbl_kind', 'copper OR steel'); COMMIT;
-- a role that bypasses the policies sees every row; so does a superuser
BEGIN; SELECT * FROM fr_check('fr_admin', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable'); COMMIT;
SELECT * FROM fr_diff('fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable');

-- the counts of a role are less than the counts of everyone
BEGIN; SET LOCAL ROLE fr_alice;
SELECT (SELECT sum(count) FROM sift.facets('fr_doc_memo', 'fr_doc_kind', 'cable', 100, true)) AS alice_sum, (SELECT count(*) FROM fr_doc WHERE memo @@> 'cable') AS alice_rows;
COMMIT;
SELECT (SELECT sum(count) FROM sift.facets('fr_doc_memo', 'fr_doc_kind', 'cable', 100, true)) AS all_sum, (SELECT count(*) FROM fr_doc WHERE memo @@> 'cable') AS all_rows;

-- a restrictive policy and a policy with a sub-select
CREATE POLICY not_secret ON fr_doc AS RESTRICTIVE USING (NOT secret);
CREATE POLICY team ON fr_doc FOR SELECT TO fr_alice USING (team IN (SELECT team FROM fr_team WHERE member = current_user));
BEGIN; SELECT * FROM fr_check('fr_alice', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable'); COMMIT;
BEGIN; SELECT * FROM fr_check('fr_bob', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable steel'); COMMIT;

-- rows changed after the index was built: new versions, a moved key, deleted rows, a write buffer
UPDATE fr_doc SET kind = 'k9' WHERE id % 7 = 0;
UPDATE fr_doc SET memo = memo || ' extra' WHERE id % 13 = 0;
DELETE FROM fr_doc WHERE id % 17 = 0;
INSERT INTO fr_doc SELECT i, 'fr_alice', 'red', 'k' || i % 3, 'cable late ' || i, false FROM generate_series(6001, 6100) i;
UPDATE fr_tbl SET kind = 'k9' WHERE id % 7 = 0;
DELETE FROM fr_tbl WHERE id % 17 = 0;
BEGIN; SELECT * FROM fr_check('fr_alice', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable'); COMMIT;
BEGIN; SELECT * FROM fr_check('fr_alice', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'late'); COMMIT;
BEGIN; SELECT * FROM fr_check('fr_alice', 'fr_tbl', 'fr_tbl_memo', 'fr_tbl_kind', 'cable'); COMMIT;
VACUUM fr_doc;
BEGIN; SELECT * FROM fr_check('fr_bob', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable'); COMMIT;

-- the owner of the table sees its rows only under FORCE ROW LEVEL SECURITY
ALTER TABLE fr_doc OWNER TO fr_bob;
GRANT USAGE ON SCHEMA fr TO fr_bob;
BEGIN; SELECT * FROM fr_check('fr_bob', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable'); COMMIT;
ALTER TABLE fr_doc FORCE ROW LEVEL SECURITY;
BEGIN; SELECT * FROM fr_check('fr_bob', 'fr_doc', 'fr_doc_memo', 'fr_doc_kind', 'cable'); COMMIT;

-- a limit and a zero limit still apply
BEGIN; SET LOCAL ROLE fr_alice;
SELECT count(*) FROM sift.facets('fr_tbl_memo', 'fr_tbl_kind', 'cable', 2);
SELECT count(*) FROM sift.facets('fr_tbl_memo', 'fr_tbl_kind', 'cable', 0);
COMMIT;

RESET search_path;
DROP SCHEMA fr CASCADE;
REVOKE USAGE ON SCHEMA sift FROM fr_alice, fr_bob, fr_admin;
DROP ROLE fr_alice;
DROP ROLE fr_bob;
DROP ROLE fr_admin;
DROP EXTENSION pg_sift;
