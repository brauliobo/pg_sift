-- sift.scope_refresh_pairs / scope_follow_pairs: a table of unique (member, scope key) pairs kept equal to a view over links_a and group membership, and a scope copy
-- built on it kept equal to the join. Member ids belong to 0 to 6 scope keys, one of them through a direct assignment and through a group at once; every edit below leaves the
-- pair table equal to the view, the copy equal to the join (sift.scope_verify) and every key answering like the join of the view.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL)
	WITH (autovacuum_enabled = false);
CREATE INDEX ON facts (dim_a_id);
INSERT INTO facts SELECT i, CASE WHEN i % 2 = 0 THEN 1 + (i / 2) % 60 ELSE 10 * ((i / 2) % 6) + 1 + (i / 12) % 5 END, timestamptz '2026-01-01' + i * interval '3 hours', i % 5, i % 97 FROM generate_series(1, 6000) i;
CREATE TABLE group_members (group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz, PRIMARY KEY (group_id, dim_a_id)) WITH (autovacuum_enabled = false);
CREATE TABLE links_a (id serial PRIMARY KEY, tenant_id int NOT NULL, group_id int, dim_a_id int, deleted_at timestamptz, expires_at timestamptz,
	CHECK ((group_id IS NULL) <> (dim_a_id IS NULL))) WITH (autovacuum_enabled = false);
-- member e is in groups 1..n(e) (n = 0 for e % 10 = 0, 1, 1, 1, 1, 1, 2, 3, 4, 6 otherwise by e % 10) and tenant g sees group g: its keys are 1..n(e)
INSERT INTO group_members SELECT g, e FROM generate_series(1, 60) e, generate_series(1, CASE e % 10 WHEN 0 THEN 0 WHEN 6 THEN 2 WHEN 7 THEN 3 WHEN 8 THEN 4 WHEN 9 THEN 6 ELSE 1 END) g;
INSERT INTO links_a (tenant_id, group_id) SELECT g, g FROM generate_series(1, 6) g;
-- direct links_a: ids of tenant 1 through the group too (one pair), and ids that are in no group
INSERT INTO links_a (tenant_id, dim_a_id) SELECT 1, e FROM generate_series(1, 60) e WHERE e % 3 = 0 AND e % 10 <> 0;
INSERT INTO links_a (tenant_id, dim_a_id) SELECT 7, e FROM generate_series(20, 60, 20) e;
CREATE VIEW visible_dim_a AS
	SELECT a.dim_a_id, a.tenant_id FROM links_a a WHERE a.dim_a_id IS NOT NULL AND a.deleted_at IS NULL AND (a.expires_at IS NULL OR a.expires_at > now())
	UNION
	SELECT m.dim_a_id, a.tenant_id FROM links_a a JOIN group_members m ON m.group_id = a.group_id AND m.deleted_at IS NULL
	WHERE a.group_id IS NOT NULL AND a.deleted_at IS NULL AND (a.expires_at IS NULL OR a.expires_at > now());
CREATE TABLE scope_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id)) WITH (autovacuum_enabled = false);

-- what a refresh and a follow need
CREATE VIEW three_columns AS SELECT dim_a_id, tenant_id, 1 AS n FROM visible_dim_a;
CREATE TABLE other_pairs (dim_a_id int NOT NULL, scope_id int NOT NULL);
CREATE TABLE text_source (id int, label text, at timestamptz);
SELECT sift.scope_refresh_pairs('scope_pairs', 'three_columns', 'key');
SELECT sift.scope_refresh_pairs('other_pairs', 'visible_dim_a', 'key');
SELECT sift.scope_refresh_pairs('scope_pairs', 'visible_dim_a', 'group');
SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'visible_dim_a', 'key', 'tenant_id');
SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'links_a', 'both', 'tenant_id');
SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'links_a', 'key', 'nope');
SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'text_source', 'key', 'label');
SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'links_a', 'key', 'tenant_id', expires => 'deleted_at_nope');
SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'text_source', 'key', 'id', expires => 'label');
SELECT sift.scope_expire_pairs('scope_pairs');
SELECT sift.scope_unfollow_pairs('scope_pairs');
SELECT count(*) AS follows FROM sift.scope_follows;
DROP TABLE other_pairs, text_source;
DROP VIEW three_columns;

-- the first follow builds the pairs from the view; the pairs of a member in no key are not there
SELECT inserted, deleted FROM sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'links_a', 'key', 'tenant_id', expires => 'expires_at');
SELECT inserted, deleted FROM sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'group_members', 'member', 'dim_a_id');
SELECT count(*) AS pairs, count(DISTINCT dim_a_id) AS ids, max(n) AS max_keys FROM (SELECT dim_a_id, count(*) n FROM scope_pairs GROUP BY 1) p;
SELECT sift.scope_create('facts_by_tenant', 'facts', 'dim_a_id', 'scope_pairs', 'tenant_id') AS copy_rows;
-- the copy is read one key at a time: a fact once in each key of its member, the same count as the join of the view
SELECT count(*) AS join_rows FROM facts f JOIN visible_dim_a v USING (dim_a_id);

CREATE FUNCTION answers(k int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE c text; j text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (SELECT kind, count(*), sum(amount) FROM facts_by_tenant WHERE tenant_id = %s GROUP BY kind) t', k) INTO c;
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (SELECT kind, count(*), sum(amount) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM visible_dim_a WHERE tenant_id = %s) GROUP BY kind) t', k) INTO j;
	RETURN CASE WHEN c IS NOT DISTINCT FROM j THEN 'same' ELSE 'DIFFERENT' END;
END $$;
CREATE FUNCTION state(label text) RETURNS TABLE (edit text, pairs bigint, pairs_differing bigint, copy_rows bigint, copy_differing bigint, keys_differing bigint) LANGUAGE sql AS $$
	SELECT label, (SELECT count(*) FROM scope_pairs),
		   (SELECT count(*) FROM ((SELECT dim_a_id, tenant_id FROM visible_dim_a EXCEPT SELECT dim_a_id, tenant_id FROM scope_pairs)
								  UNION ALL (SELECT dim_a_id, tenant_id FROM scope_pairs EXCEPT SELECT dim_a_id, tenant_id FROM visible_dim_a)) d),
		   (SELECT count(*) FROM facts_by_tenant), (SELECT count(*) FROM sift.scope_verify('facts_by_tenant')),
		   (SELECT count(*) FROM generate_series(1, 9) k WHERE answers(k) <> 'same') $$;

CREATE TABLE edits (n serial, label text, stmt text);
INSERT INTO edits (label, stmt) VALUES
	('a tenant is assigned a group', 'INSERT INTO links_a (tenant_id, group_id) VALUES (8, 3)'),
	('a direct assignment of an id the group already gives (one pair)', 'INSERT INTO links_a (tenant_id, dim_a_id) VALUES (8, 3), (8, 13)'),
	('the group assignment is soft deleted (the direct ones stay)', 'UPDATE links_a SET deleted_at = now() WHERE tenant_id = 8 AND group_id = 3'),
	('the group assignment is restored', 'UPDATE links_a SET deleted_at = NULL WHERE tenant_id = 8 AND group_id = 3'),
	('a direct assignment of an id that also comes through its group goes (the pair stays)', 'DELETE FROM links_a WHERE tenant_id = 1 AND dim_a_id = 3'),
	('the group membership of that id is soft deleted (the pair goes)', 'UPDATE group_members SET deleted_at = now() WHERE group_id = 1 AND dim_a_id = 3'),
	('the membership is restored', 'UPDATE group_members SET deleted_at = NULL WHERE group_id = 1 AND dim_a_id = 3'),
	('an id moves to another group', 'UPDATE group_members SET group_id = 6 WHERE group_id = 2 AND dim_a_id IN (7, 17)'),
	('the group of an id in 6 keys is deleted from its memberships', 'DELETE FROM group_members WHERE dim_a_id = 9'),
	('an id in no key joins a group', 'INSERT INTO group_members VALUES (4, 10), (5, 10), (6, 10)'),
	('a group assignment moves to another tenant', 'UPDATE links_a SET tenant_id = 9 WHERE tenant_id = 5 AND group_id = 5'),
	('an assignment turns from a group to a member', 'UPDATE links_a SET group_id = NULL, dim_a_id = 50 WHERE tenant_id = 9'),
	('an update that changes nothing', 'UPDATE links_a SET deleted_at = deleted_at'),
	('an assignment that has already expired gives no pair', 'INSERT INTO links_a (tenant_id, group_id, expires_at) VALUES (2, 4, now() - interval ''1 day'')'),
	('an assignment that expires later gives its pairs', 'INSERT INTO links_a (tenant_id, dim_a_id, expires_at) VALUES (9, 1, now() + interval ''1 day''), (9, 2, now() + interval ''1 day'')'),
	('links_a and memberships change in one statement',
	 'WITH g AS (DELETE FROM group_members WHERE group_id = 3 AND dim_a_id IN (8, 9, 18) RETURNING 1) INSERT INTO links_a (tenant_id, group_id) VALUES (3, 6)'),
	('links_a and memberships change in one statement, the other way',
	 'WITH a AS (UPDATE links_a SET deleted_at = now() WHERE tenant_id = 4 RETURNING 1) INSERT INTO group_members VALUES (4, 11), (4, 12)'),
	('the memberships are truncated', 'TRUNCATE group_members'),
	('the memberships return', 'INSERT INTO group_members SELECT g, e FROM generate_series(1, 60) e, generate_series(1, CASE e % 10 WHEN 0 THEN 0 WHEN 6 THEN 2 WHEN 7 THEN 3 WHEN 8 THEN 4 WHEN 9 THEN 6 ELSE 1 END) g'),
	('the links_a are truncated', 'TRUNCATE links_a'),
	('the links_a return', 'INSERT INTO links_a (tenant_id, group_id) SELECT g, g FROM generate_series(1, 6) g'),
	('a fact of an id in 6 keys is added', 'INSERT INTO facts VALUES (900000, 9, now(), 1, 5)'),
	('facts of ids that are in no group yet', 'INSERT INTO facts SELECT 910000 + i, 1000 + i, now(), i % 5, i FROM generate_series(1, 20) i'),
	('a group gains 1,000 ids', 'INSERT INTO group_members SELECT 3, e FROM generate_series(1001, 2000) e'),
	('1,000 ids are soft deleted', 'UPDATE group_members SET deleted_at = now() WHERE group_id = 3 AND dim_a_id > 1000'),
	('and restored', 'UPDATE group_members SET deleted_at = NULL WHERE group_id = 3 AND dim_a_id > 1000'),
	('a group loses 1,000 ids (hard delete)', 'DELETE FROM group_members WHERE group_id = 3 AND dim_a_id > 1000'),
	('an id moves between groups (one row)', 'UPDATE group_members SET group_id = 6 WHERE group_id = 4 AND dim_a_id = 8'),
	('a tenant loses a group (hard delete)', 'DELETE FROM links_a WHERE tenant_id = 8 AND group_id = 3'),
	('a tenant gains a group again', 'INSERT INTO links_a (tenant_id, group_id) VALUES (8, 3)'),
	('a direct assignment moves to another tenant', 'UPDATE links_a SET tenant_id = 2 WHERE tenant_id = 7 AND dim_a_id = 20'),
	('a direct assignment names another id', 'UPDATE links_a SET dim_a_id = 31 WHERE tenant_id = 2 AND dim_a_id = 20'),
	('an assignment of an id its group gives is soft deleted (no pair changes)', 'UPDATE links_a SET deleted_at = now() WHERE tenant_id = 1 AND dim_a_id = 6'),
	('and restored (no pair changes)', 'UPDATE links_a SET deleted_at = NULL WHERE tenant_id = 1 AND dim_a_id = 6');
CREATE FUNCTION run_edits() RETURNS TABLE (edit text, pairs bigint, pairs_differing bigint, copy_rows bigint, copy_differing bigint, keys_differing bigint) LANGUAGE plpgsql AS $$
DECLARE e record;
BEGIN
	RETURN QUERY SELECT * FROM state('start');
	FOR e IN SELECT * FROM edits ORDER BY n LOOP
		EXECUTE e.stmt;
		RETURN QUERY SELECT * FROM state(e.label);
	END LOOP;
END $$;
-- TRUNCATE rebuilds the index of the copy and says what it chose each time
SET client_min_messages = warning;
SELECT * FROM run_edits();
RESET client_min_messages;

-- a refresh named by member ids or scope keys repairs those and no others; the whole refresh repairs all (what a disabled trigger or a COPY with triggers off leaves behind)
ALTER TABLE links_a DISABLE TRIGGER USER;
ALTER TABLE group_members DISABLE TRIGGER USER;
INSERT INTO links_a (tenant_id, group_id) VALUES (8, 1);
INSERT INTO links_a (tenant_id, dim_a_id) VALUES (9, 40);
SELECT edit, pairs, pairs_differing, copy_differing FROM state('two edits with the triggers off');
SELECT inserted, deleted FROM sift.scope_refresh_pairs('scope_pairs', 'visible_dim_a', 'key', ARRAY[8]);
SELECT edit, pairs, pairs_differing, copy_differing FROM state('tenant 8 refreshed');
SELECT inserted, deleted FROM sift.scope_refresh_pairs('scope_pairs', 'visible_dim_a', 'member', ARRAY[40]);
SELECT edit, pairs, pairs_differing, copy_differing, keys_differing FROM state('member 40 refreshed');
SELECT inserted, deleted FROM sift.scope_refresh_pairs('scope_pairs', 'visible_dim_a', 'key');
ALTER TABLE links_a ENABLE TRIGGER USER;
ALTER TABLE group_members ENABLE TRIGGER USER;

-- expiry by date with no write: the pair stays until the sweep, which applies what expired since the last one
INSERT INTO links_a (tenant_id, dim_a_id, expires_at) VALUES (9, 51, clock_timestamp() + interval '1 second'), (9, 52, clock_timestamp() + interval '1 second');
SELECT edit, pairs, pairs_differing, copy_differing FROM state('two pairs that expire in a second');
SELECT pg_sleep(1.2);
SELECT edit, pairs, pairs_differing, copy_differing FROM state('after the date, before the sweep');
SELECT inserted, deleted FROM sift.scope_expire_pairs('scope_pairs');
SELECT edit, pairs, pairs_differing, copy_differing, keys_differing FROM state('after the sweep');
SELECT inserted, deleted FROM sift.scope_expire_pairs('scope_pairs');
-- a sweep with a window that does not reach back to the expiry does not see it: the overlap is what a long transaction needs
UPDATE sift.scope_follows SET swept_at = now() + interval '1 hour';
INSERT INTO links_a (tenant_id, dim_a_id, expires_at) VALUES (9, 53, clock_timestamp() + interval '1 second');
SELECT pg_sleep(1.2);
SELECT inserted, deleted FROM sift.scope_expire_pairs('scope_pairs', interval '0');
SELECT inserted, deleted FROM sift.scope_expire_pairs('scope_pairs', interval '3 hours');
SELECT edit, pairs, pairs_differing, copy_differing, keys_differing FROM state('after the sweep with an overlap');

-- the same edits with links_a followed by pair: a row that names its member and its key refreshes that pair, a group row its key, a membership its member
CREATE FUNCTION seed() RETURNS void LANGUAGE sql AS $$
	DELETE FROM facts WHERE id >= 900000;
	TRUNCATE group_members;
	TRUNCATE links_a RESTART IDENTITY;
	INSERT INTO group_members SELECT g, e FROM generate_series(1, 60) e, generate_series(1, CASE e % 10 WHEN 0 THEN 0 WHEN 6 THEN 2 WHEN 7 THEN 3 WHEN 8 THEN 4 WHEN 9 THEN 6 ELSE 1 END) g;
	INSERT INTO links_a (tenant_id, group_id) SELECT g, g FROM generate_series(1, 6) g;
	INSERT INTO links_a (tenant_id, dim_a_id) SELECT 1, e FROM generate_series(1, 60) e WHERE e % 3 = 0 AND e % 10 <> 0;
	INSERT INTO links_a (tenant_id, dim_a_id) SELECT 7, e FROM generate_series(20, 60, 20) e $$;
SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'links_a', 'pair', 'dim_a_id');
CREATE TABLE one_column (tenant_id int);
SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'one_column', 'pair', 'tenant_id');
DROP TABLE one_column;
SELECT sift.scope_unfollow_pairs('scope_pairs', 'links_a');
SELECT inserted, deleted FROM sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'links_a', 'pair', 'tenant_id', expires => 'expires_at');
SET client_min_messages = warning;
SELECT seed();
SELECT * FROM run_edits();
RESET client_min_messages;

-- random edits of both sources, by key and by pair: the pairs equal the view after every statement
CREATE FUNCTION random_edits(steps int) RETURNS TABLE (statements int, drifting int) LANGUAGE plpgsql AS $$
DECLARE i int; k int; g int; e int; d int := 0; stmt text;
BEGIN
	PERFORM setseed(0.42);
	FOR i IN 1..steps LOOP
		k := 1 + (random() * 8)::int; g := 1 + (random() * 5)::int; e := 1 + (random() * 59)::int;
		stmt := (ARRAY[
			format('INSERT INTO links_a (tenant_id, dim_a_id, expires_at) VALUES (%s, %s, CASE WHEN random() < 0.2 THEN now() - interval ''1 day'' END)', k, e),
			format('INSERT INTO links_a (tenant_id, group_id) VALUES (%s, %s)', k, g),
			format('UPDATE links_a SET deleted_at = CASE WHEN deleted_at IS NULL THEN now() END WHERE tenant_id = %s AND (dim_a_id = %s OR group_id = %s)', k, e, g),
			format('UPDATE links_a SET tenant_id = %s WHERE id = (SELECT id FROM links_a ORDER BY random() LIMIT 1)', k),
			format('UPDATE links_a SET dim_a_id = %s, group_id = NULL WHERE id = (SELECT id FROM links_a ORDER BY random() LIMIT 1)', e),
			'DELETE FROM links_a WHERE id = (SELECT id FROM links_a ORDER BY random() LIMIT 1)',
			format('INSERT INTO group_members VALUES (%s, %s) ON CONFLICT DO NOTHING', g, e),
			format('UPDATE group_members SET deleted_at = CASE WHEN deleted_at IS NULL THEN now() END WHERE group_id = %s AND dim_a_id = %s', g, e),
			format('DELETE FROM group_members WHERE group_id = %s AND dim_a_id %% 7 = %s', g, e % 7)])[1 + floor(random() * 9)::int];
		EXECUTE stmt;
		d := d + (SELECT (SELECT count(*) FROM ((SELECT dim_a_id, tenant_id FROM visible_dim_a EXCEPT SELECT dim_a_id, tenant_id FROM scope_pairs)
												UNION ALL (SELECT dim_a_id, tenant_id FROM scope_pairs EXCEPT SELECT dim_a_id, tenant_id FROM visible_dim_a)) x) > 0)::int;
	END LOOP;
	RETURN QUERY SELECT steps, d;
END $$;
SELECT * FROM random_edits(300);
SELECT sift.scope_unfollow_pairs('scope_pairs', 'links_a');
SELECT inserted, deleted FROM sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'links_a', 'key', 'tenant_id', expires => 'expires_at');
SELECT * FROM random_edits(300);
SELECT edit, pairs_differing, copy_differing, keys_differing FROM state('after the random edits');

-- the copy is read by its sift index, with the answers of the normal plan
CREATE EXTENSION pg_sift_helpers;
VACUUM (FREEZE, ANALYZE) facts_by_tenant;
SELECT sift_helpers.differences_agg('SELECT kind, count(*), sum(amount) FROM facts_by_tenant WHERE tenant_id = 3 GROUP BY kind') AS differences;

-- stop following: the pairs stay, the triggers go, the copy keeps the triggers of its own
SELECT sift.scope_unfollow_pairs('scope_pairs', 'group_members');
SELECT source, source_column FROM sift.scope_follows ORDER BY 1;
SELECT sift.scope_unfollow_pairs('scope_pairs', 'group_members');
SELECT sift.scope_unfollow_pairs('scope_pairs');
SELECT count(*) AS follows FROM sift.scope_follows;
SELECT count(*) AS follow_triggers FROM pg_trigger WHERE NOT tgisinternal AND tgrelid IN ('links_a'::regclass, 'group_members'::regclass);
DELETE FROM links_a WHERE tenant_id = 8;
SELECT edit, pairs_differing, copy_differing FROM state('a source change after the unfollow is not seen');
SELECT sift.scope_drop('facts_by_tenant');
DROP FUNCTION run_edits(), state(text), answers(int), seed(), random_edits(int);
DROP TABLE edits, facts, scope_pairs, links_a, group_members CASCADE;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
