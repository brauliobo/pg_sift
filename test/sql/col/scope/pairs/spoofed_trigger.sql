-- sift.scope_follow_trg runs as the owner of the extension. A role that follows its own table and puts the trigger function on a trigger that declares no transition
-- table must not make it read a relation of pg_temp named new_rows or old_rows (S116-03): the trigger refuses, and a follow made by scope_follow_pairs still works.
CREATE EXTENSION pg_sift;
\set VERBOSITY sqlstate
SET client_min_messages = warning;
DO $$ BEGIN IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'sfs_atk') THEN DROP OWNED BY sfs_atk CASCADE; DROP ROLE sfs_atk; END IF; END $$;
RESET client_min_messages;
CREATE ROLE sfs_atk;
CREATE SCHEMA sfs AUTHORIZATION sfs_atk;
SET SESSION AUTHORIZATION sfs_atk;
SET search_path = sfs;
CREATE TABLE ran (who name);
CREATE TABLE src (member int, tenant int);
CREATE TABLE pairs (member int, tenant int, UNIQUE (member, tenant));
CREATE VIEW pairs_v AS SELECT member, tenant FROM src;
SELECT * FROM sift.scope_follow_pairs('pairs', 'pairs_v', 'src', 'member', 'member');

-- the follow made by scope_follow_pairs keeps the pairs on insert, update, delete and truncate
INSERT INTO src VALUES (1, 1), (2, 2);
UPDATE src SET tenant = 3 WHERE member = 2;
DELETE FROM src WHERE member = 1;
SELECT * FROM pairs ORDER BY member;
TRUNCATE src;
SELECT count(*) FROM pairs;

-- the same triggers with no transition table, and relations of pg_temp under their names that run code of the role
CREATE FUNCTION pg_temp.pwn() RETURNS int LANGUAGE plpgsql AS $$ BEGIN INSERT INTO sfs.ran VALUES (current_user); RETURN 1; END $$;
CREATE TEMP VIEW new_rows AS SELECT pg_temp.pwn() AS member, 1 AS tenant;
CREATE TEMP VIEW old_rows AS SELECT pg_temp.pwn() AS member, 1 AS tenant;
SELECT tgname AS ins FROM pg_trigger WHERE tgrelid = 'src'::regclass AND tgname LIKE '%\_sins' \gset
DO $$
DECLARE
	t name;
BEGIN
	FOR t IN SELECT tgname FROM pg_trigger WHERE tgrelid = 'sfs.src'::regclass AND tgname ~ '_s(ins|upd|del)$' LOOP
		EXECUTE format('DROP TRIGGER %I ON sfs.src', t);
		EXECUTE format('CREATE TRIGGER %I AFTER %s ON sfs.src FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_follow_trg()', t,
					   CASE right(t, 3) WHEN 'ins' THEN 'INSERT' WHEN 'upd' THEN 'UPDATE' ELSE 'DELETE' END);
	END LOOP;
END $$;
INSERT INTO src VALUES (5, 5);
UPDATE src SET tenant = 6;
DELETE FROM src;
-- a trigger that declares other transition tables than the follow's
DROP TRIGGER :"ins" ON src;
CREATE TRIGGER :"ins" AFTER INSERT ON src REFERENCING NEW TABLE AS other FOR EACH STATEMENT EXECUTE FUNCTION sift.scope_follow_trg();
INSERT INTO src VALUES (5, 5);
RESET SESSION AUTHORIZATION;
SELECT count(*) AS ran_as_superuser FROM sfs.ran r JOIN pg_roles a ON a.rolname = r.who WHERE a.rolsuper;
SELECT count(*) AS ran FROM sfs.ran;

-- following the table again makes its triggers right
SET SESSION AUTHORIZATION sfs_atk;
SET search_path = sfs;
SELECT sift.scope_unfollow_pairs('pairs');
SELECT * FROM sift.scope_follow_pairs('pairs', 'pairs_v', 'src', 'member', 'member');
INSERT INTO src VALUES (7, 7), (8, 8);
UPDATE src SET tenant = 9 WHERE member = 8;
DELETE FROM src WHERE member = 7;
SELECT * FROM pairs ORDER BY member;
SELECT count(*) AS ran FROM ran;
RESET SESSION AUTHORIZATION;
DROP OWNED BY sfs_atk CASCADE;
DROP ROLE sfs_atk;
DROP EXTENSION pg_sift;
