-- Row-level security around a BM25 index: the violation of a UNIQUE index names no key to a role that policies limit (the key may be a row it cannot see), and the read of
-- sift.facets() that applies the caller's policies takes the equality of tid from pg_catalog whatever the search_path holds.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE ROLE rh_bob;
CREATE SCHEMA rh;
GRANT USAGE ON SCHEMA rh, sift TO rh_bob;
SET search_path = rh, public;

CREATE TABLE rh_doc (id int PRIMARY KEY, owner text NOT NULL, doc text, kind text);
CREATE UNIQUE INDEX rh_doc_doc ON rh_doc USING sift (doc);
CREATE INDEX rh_doc_kind ON rh_doc USING sift (kind) WITH (analyzer = keyword);
INSERT INTO rh_doc SELECT i, CASE WHEN i % 2 = 0 THEN 'rh_alice' ELSE 'rh_bob' END, 'alpha secret ' || i, 'k' || i % 3 FROM generate_series(1, 40) i;
GRANT SELECT, INSERT ON rh_doc TO rh_bob;
ALTER TABLE rh_doc ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON rh_doc USING (owner = current_user);

-- the owner sees the key; rh_bob, whose policy hides the row of the key, sees none
INSERT INTO rh_doc VALUES (100, 'rh_alice', 'alpha secret 2', 'k0');
SET ROLE rh_bob;
INSERT INTO rh_doc VALUES (101, 'rh_bob', 'alpha secret 2', 'k0');
INSERT INTO rh_doc VALUES (102, 'rh_bob', 'alpha secret 3', 'k0');
RESET ROLE;

-- an equality of tid that a schema ahead of pg_catalog holds is not the one the policy read calls
CREATE SCHEMA rh_evil;
GRANT USAGE ON SCHEMA rh_evil TO rh_bob;
CREATE FUNCTION rh_evil.tideq(tid, tid) RETURNS boolean LANGUAGE plpgsql AS $$ BEGIN RAISE WARNING 'rh_evil.tideq called'; RETURN true; END $$;
CREATE OPERATOR rh_evil.= (LEFTARG = tid, RIGHTARG = tid, FUNCTION = rh_evil.tideq);
BEGIN;
SET LOCAL ROLE rh_bob;
SET LOCAL search_path = rh_evil, pg_catalog, rh;
SELECT * FROM sift.facets('rh.rh_doc_doc', 'rh.rh_doc_kind', 'alpha', 10, true);
COMMIT;
BEGIN;
SET LOCAL ROLE rh_bob;
SET LOCAL search_path = '';
SELECT * FROM sift.facets('rh.rh_doc_doc', 'rh.rh_doc_kind', 'alpha', 10, true);
SELECT kind, count(*) FROM rh.rh_doc WHERE doc @@> 'alpha' GROUP BY 1 ORDER BY 2 DESC, 1;
COMMIT;

RESET search_path;
DROP SCHEMA rh_evil CASCADE;
DROP SCHEMA rh CASCADE;
REVOKE USAGE ON SCHEMA sift FROM rh_bob;
DROP EXTENSION pg_sift;
DROP ROLE rh_bob;
