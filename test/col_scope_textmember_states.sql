-- shared by col_scope_textmember and col_scope_textmember_collide: the fixtures, the forms and the states of the pairs and of the facts
CREATE TABLE events (id int PRIMARY KEY, account text, created_at date, kind text, amount int) WITH (autovacuum_enabled = false);
-- accounts 'acct-1' to 'acct-400', one in 97 with no account (NULL), one in 89 with the empty account
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN '' ELSE 'acct-' || (1 + (i::bigint * 7919) % 400) END,
       date '2025-01-01' + i % 300, 'k' || i % 4, i % 100 FROM generate_series(1, 6000) i;
-- accounts 1 to 360 are members of tenant 1 + n % 10, one in seven of a second tenant; the empty account is in tenant 3; accounts that differ in the bytes: a case, a space, an accent, a long one
CREATE TABLE pairs (account text NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (account, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO pairs SELECT 'acct-' || i, 1 + i % 10 FROM generate_series(1, 360) i;
INSERT INTO pairs SELECT 'acct-' || i, 1 + (i * 3 + 1) % 10 FROM generate_series(1, 360) i WHERE i % 7 = 0 ON CONFLICT DO NOTHING;
INSERT INTO pairs SELECT a, 4 FROM unnest(ARRAY['', 'Acct-4', 'acct-4 ', 'ácct-4', repeat('long-', 400)]) a;
UPDATE pairs SET tenant_id = 3 WHERE account = '';
CREATE INDEX pairs_journal ON pairs USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX events_col ON events USING sift_col (id, account, created_at, kind, amount) WITH (scope = 'tenant_id: account -> pairs(account, tenant_id)');
REINDEX INDEX events_col;
-- a long account on a fact too
INSERT INTO events VALUES (6001, repeat('long-', 400), date '2025-03-01', 'k1', 7), (6002, 'Acct-4', date '2025-03-02', 'k2', 8), (6003, 'acct-4 ', date '2025-03-03', 'k3', 9), (6004, 'ácct-4', date '2025-03-04', 'k0', 10);
REINDEX INDEX events_col;

-- a varchar fact column with a text member of the pairs (the bytes decide both), a uuid member, and a text member with a text key
CREATE TABLE var_events (id int PRIMARY KEY, account varchar(2000), kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO var_events SELECT id, account, kind, amount FROM events;
CREATE INDEX var_events_col ON var_events USING sift_col (id, account, kind, amount) WITH (scope = 'tenant_id: account -> pairs(account, tenant_id)');
REINDEX INDEX var_events_col;
CREATE TABLE uuid_pairs (account uuid NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (account, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO uuid_pairs SELECT md5(account)::uuid, tenant_id FROM pairs;
CREATE INDEX uuid_pairs_journal ON uuid_pairs USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE uuid_events (id int PRIMARY KEY, account uuid, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO uuid_events SELECT id, md5(account)::uuid, kind, amount FROM events;
CREATE INDEX uuid_events_col ON uuid_events USING sift_col (id, account, kind, amount) WITH (scope = 'tenant_id: account -> uuid_pairs(account, tenant_id)');
REINDEX INDEX uuid_events_col;
CREATE TABLE txt_pairs (account text NOT NULL, tenant_id text NOT NULL, PRIMARY KEY (account, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO txt_pairs SELECT account, 'tenant-' || tenant_id FROM pairs;
CREATE INDEX txt_pairs_journal ON txt_pairs USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE txt_events (id int PRIMARY KEY, account text, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO txt_events SELECT id, account, kind, amount FROM events;
CREATE INDEX txt_events_col ON txt_events USING sift_col (id, account, kind, amount) WITH (scope = 'tenant_id: account -> txt_pairs(account, tenant_id)');
REINDEX INDEX txt_events_col;
VACUUM (FREEZE, ANALYZE) events, pairs, var_events, uuid_events, uuid_pairs, txt_events, txt_pairs;

-- the scope segments of an index with a text, varchar or uuid member are column format 50 (an older library refuses them: its journal records words of bytes)
SELECT i AS index, format, count(*) AS segments FROM (VALUES ('events_col'), ('var_events_col'), ('uuid_events_col'), ('txt_events_col')) v (i), LATERAL sift.col_segments(i::regclass) GROUP BY 1, 2 ORDER BY 1, 2;
-- the members of the pairs and of the facts, by value: the stored entries are those of the pairs as of the build
SELECT count(*) AS entries, count(DISTINCT scope) AS scopes FROM sift._test_col_scope_entries('events_col');
SELECT (SELECT count(*) FROM events WHERE account IS NULL) AS null_facts, (SELECT count(*) FROM events WHERE account = '') AS empty_facts, (SELECT count(*) FROM pairs WHERE account = '') AS empty_pairs;

CREATE TABLE forms (name text PRIMARY KEY, q text);
INSERT INTO forms VALUES
 ('IN, one key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.account IN (SELECT account FROM pairs WHERE tenant_id = 4) GROUP BY kind$q$),
 ('IN, a list', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.account IN (SELECT account FROM pairs WHERE tenant_id IN (4, 5, 6)) GROUP BY kind$q$),
 ('IN, a key nobody has', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.account IN (SELECT account FROM pairs WHERE tenant_id = 99) GROUP BY kind$q$),
 ('IN, the tenant of the empty account', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE e.account IN (SELECT account FROM pairs WHERE tenant_id = 3) GROUP BY kind$q$),
 ('EXISTS', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e WHERE EXISTS (SELECT 1 FROM pairs s WHERE s.account = e.account AND s.tenant_id = 4) GROUP BY kind$q$),
 ('join, one key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN pairs s ON s.account = e.account WHERE s.tenant_id = 4 GROUP BY kind$q$),
 ('join, a list, a row for each pair', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM events e JOIN pairs s ON s.account = e.account WHERE s.tenant_id IN (4, 5) GROUP BY kind$q$),
 ('group by the key, every key', $q$SELECT s.tenant_id, count(*) AS n, sum(amount) AS total FROM events e JOIN pairs s ON s.account = e.account GROUP BY s.tenant_id$q$),
 ('group by the key and a column', $q$SELECT s.tenant_id, kind, count(*) AS n FROM events e JOIN pairs s ON s.account = e.account WHERE s.tenant_id IN (1, 2, 99) GROUP BY s.tenant_id, kind$q$),
 ('every fact with a pair', $q$SELECT count(*) AS n, sum(amount) AS total FROM events e WHERE EXISTS (SELECT 1 FROM pairs s WHERE s.account = e.account)$q$),
 ('varchar facts, text pairs', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM var_events e WHERE e.account IN (SELECT account FROM pairs WHERE tenant_id IN (4, 5)) GROUP BY kind$q$),
 ('varchar facts, group by the key', $q$SELECT s.tenant_id, count(*) AS n FROM var_events e JOIN pairs s ON s.account = e.account GROUP BY s.tenant_id$q$),
 ('uuid member', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM uuid_events e WHERE e.account IN (SELECT account FROM uuid_pairs WHERE tenant_id IN (4, 5)) GROUP BY kind$q$),
 ('uuid member, group by the key', $q$SELECT s.tenant_id, count(*) AS n FROM uuid_events e JOIN uuid_pairs s ON s.account = e.account GROUP BY s.tenant_id$q$),
 ('text member, text key', $q$SELECT kind, sum(amount) AS total, count(*) AS n FROM txt_events e WHERE e.account IN (SELECT account FROM txt_pairs WHERE tenant_id IN ('tenant-4', 'tenant-5')) GROUP BY kind$q$),
 ('text member, text key, group by the key', $q$SELECT s.tenant_id, count(*) AS n FROM txt_events e JOIN txt_pairs s ON s.account = e.account GROUP BY s.tenant_id$q$);

-- the plan: the scope read (its keys and semantics), or the join and the reason the scope pass gave (sift.explain_why, arm scope), else the plan
CREATE FUNCTION label(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text; plan text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF plan ~ 'Scope Index: ' THEN
		RETURN 'scope read, ' || substring(plan from 'Scope Semantics: ([^\n]*)');
	END IF;
	SELECT regexp_replace(reason, '^.*the join runs as written \((.*)\)$', '\1') INTO r FROM sift.explain_why(q) WHERE arm = 'scope';
	RETURN CASE WHEN r IS NOT NULL THEN 'join, ' || r WHEN plan ~ 'Lookup Joins' THEN 'join' WHEN plan ~ 'SiftAgg' THEN 'SiftAgg' ELSE 'normal plan' END;
END $$;

-- the rows of the statement as planned, with sift.agg_scope off and with sift.enable_agg off (the normal plan)
CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS scope_rows;
	EXECUTE 'CREATE TEMP TABLE scope_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM scope_rows t;
	DROP TABLE scope_rows;
	RETURN r;
END $$;
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; c text;
BEGIN
	a := rows_md5(q);
	PERFORM set_config('sift.agg_scope', 'off', false);
	b := rows_md5(q);
	PERFORM set_config('sift.enable_agg', 'off', false);
	c := rows_md5(q);
	PERFORM set_config('sift.agg_scope', 'on', false);
	PERFORM set_config('sift.enable_agg', 'on', false);
	RETURN a = b AND b = c;
END $$;

-- what the run did: the reader's refusal, the members it listed, else the units
CREATE FUNCTION run_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope (Fallback|Changed Keys|Patched Keys|Dedupe Keys|Members Listed)' THEN
			r := r || regexp_replace(l, '^ *Scope ', '') || '; ';
		END IF;
	END LOOP;
	RETURN CASE WHEN r = '' THEN 'units' ELSE rtrim(r, '; ') END;
END $$;

SELECT name, label(q) AS plan, same(q) AS same_rows FROM forms ORDER BY name COLLATE "C";
SELECT name, run_of(q) AS run FROM forms ORDER BY name COLLATE "C";

CREATE FUNCTION states() RETURNS TABLE (name text, same_rows bool) LANGUAGE sql AS $$
	SELECT f.name, same(f.q) FROM forms f ORDER BY f.name COLLATE "C"
$$;

-- the pairs change: a pair added (a known member, a member no fact has), removed, a pair re-keyed, a member renamed, a member that leaves every pair
INSERT INTO pairs VALUES ('acct-2', 4), ('acct-new', 4), ('acct-401', 4);
INSERT INTO uuid_pairs SELECT md5(account)::uuid, tenant_id FROM pairs WHERE account IN ('acct-2', 'acct-new', 'acct-401') AND tenant_id = 4;
INSERT INTO txt_pairs VALUES ('acct-2', 'tenant-4'), ('acct-new', 'tenant-4'), ('acct-401', 'tenant-4');
SELECT 'added' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();
SELECT name, run_of(q) AS run FROM forms WHERE name IN ('IN, one key', 'join, a list, a row for each pair', 'uuid member', 'text member, text key') ORDER BY name COLLATE "C";

DELETE FROM pairs WHERE tenant_id = 4 AND account ~ '^acct-[0-9]*5$';
DELETE FROM uuid_pairs WHERE tenant_id = 4 AND account IN (SELECT md5('acct-' || i)::uuid FROM generate_series(1, 400) i WHERE i % 10 = 5);
DELETE FROM txt_pairs WHERE tenant_id = 'tenant-4' AND account ~ '^acct-[0-9]*5$';
SELECT 'removed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();

UPDATE pairs SET tenant_id = 5 WHERE tenant_id = 6 AND account ~ '^acct-[0-9]*[02468]$';
UPDATE txt_pairs SET tenant_id = 'tenant-5' WHERE tenant_id = 'tenant-6' AND account ~ '^acct-[0-9]*[02468]$';
SELECT 're-keyed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();

UPDATE pairs SET account = 'acct-renamed-' || account WHERE tenant_id = 7 AND account ~ '^acct-[0-9]*3$';
UPDATE txt_pairs SET account = 'acct-renamed-' || account WHERE tenant_id = 'tenant-7' AND account ~ '^acct-[0-9]*3$';
SELECT 'renamed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();

DELETE FROM pairs WHERE tenant_id = 4;
DELETE FROM txt_pairs WHERE tenant_id = 'tenant-4';
DELETE FROM uuid_pairs WHERE tenant_id = 4;
SELECT 'key gone' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();

-- the facts change: a fact re-assigned to another account, a fact with no account, with the empty one, new facts
UPDATE events SET account = 'acct-7' WHERE id % 50 = 0;
UPDATE events SET account = NULL WHERE id % 61 = 0;
UPDATE events SET account = '' WHERE id % 73 = 0;
INSERT INTO events SELECT 6100 + i, 'acct-' || (1 + i % 360), date '2025-06-01', 'k0', 5 FROM generate_series(1, 300) i;
DELETE FROM var_events;
DELETE FROM uuid_events;
DELETE FROM txt_events;
INSERT INTO var_events SELECT id, account, kind, amount FROM events;
INSERT INTO uuid_events SELECT id, md5(account)::uuid, kind, amount FROM events;
INSERT INTO txt_events SELECT id, account, kind, amount FROM events;
SELECT 'facts changed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();
SELECT name, run_of(q) AS run FROM forms WHERE name IN ('IN, one key', 'IN, a list', 'varchar facts, text pairs') ORDER BY name COLLATE "C";

-- the stored units and the join agree after the pairs are merged into the index (REINDEX), and read the units again
VACUUM events, pairs, var_events, uuid_events, uuid_pairs, txt_events, txt_pairs;
SELECT 'vacuumed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();
REINDEX INDEX events_col;
REINDEX INDEX var_events_col;
REINDEX INDEX uuid_events_col;
REINDEX INDEX txt_events_col;
SELECT 'reindexed' AS state, count(*) FILTER (WHERE NOT same_rows) AS differing, count(*) AS forms FROM states();
SELECT name, run_of(q) AS run FROM forms ORDER BY name COLLATE "C";

SELECT sift.col_verify('events_col', true), sift.col_verify('var_events_col', true), sift.col_verify('uuid_events_col', true), sift.col_verify('txt_events_col', true);

DROP FUNCTION states(), same(text), rows_md5(text), run_of(text), label(text);
DROP TABLE forms, events, pairs, var_events, uuid_events, uuid_pairs, txt_events, txt_pairs CASCADE;
