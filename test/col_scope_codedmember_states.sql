-- shared by col_scope_codedmember and col_scope_codedmember_collide: the fixtures, and the answers of a scope read of the entity rows by a key, for a text, varchar and uuid member (and a text key with a text member), in every state of the pairs and of the facts
-- accounts 'acct-1' to 'acct-60' have 400 facts each, in the order of their time, so a unit holds the facts of one or two of them; accounts that differ in their bytes only (a case, a trailing space, an accent), the empty one, a long one and one no pair has at first come after
CREATE TABLE events (id int PRIMARY KEY, account text NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 'acct-' || (1 + (i - 1) / 400), timestamptz '2025-01-01' + i * interval '17 minutes', i % 7, (i * 37) % 1000 FROM generate_series(1, 24000) i;
INSERT INTO events SELECT 24000 + 40 * (n - 1) + j, a, timestamptz '2025-01-01' + (24000 + 40 * (n - 1) + j) * interval '17 minutes', j % 7, j
  FROM unnest(ARRAY['Acct-4', 'acct-4 ', 'ácct-4', repeat('long-', 400), 'acct-61', '']) WITH ORDINALITY u (a, n), generate_series(1, 40) j;
CREATE TABLE var_events (id int PRIMARY KEY, account varchar(2000) NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO var_events SELECT * FROM events;
CREATE TABLE uuid_events (id int PRIMARY KEY, account uuid NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO uuid_events SELECT id, md5(account)::uuid, created_at, kind, v FROM events;
CREATE TABLE key_events (id int PRIMARY KEY, account text NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO key_events SELECT * FROM events;

-- tenants 1 to 4 have the accounts 1 to 60 by their number, tenant 5 the accounts 10 to 40, tenant 6 every third account; tenant 4 has the accounts that differ in their bytes too
CREATE TABLE pairs_t (account text NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (account, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO pairs_t SELECT 'acct-' || m, 1 + m % 4 FROM generate_series(1, 60) m;
INSERT INTO pairs_t SELECT 'acct-' || m, 5 FROM generate_series(10, 40) m;
INSERT INTO pairs_t SELECT 'acct-' || m, 6 FROM generate_series(1, 60) m WHERE m % 3 = 0;
INSERT INTO pairs_t SELECT a, 4 FROM unnest(ARRAY['Acct-4', 'acct-4 ', 'ácct-4', repeat('long-', 400), '']) a;
CREATE TABLE pairs_u (account uuid NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (account, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO pairs_u SELECT md5(account)::uuid, tenant_id FROM pairs_t;
CREATE TABLE pairs_k (account text NOT NULL, tenant_id text NOT NULL, PRIMARY KEY (account, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO pairs_k SELECT account, 'tenant-' || tenant_id FROM pairs_t;
CREATE INDEX pairs_t_journal ON pairs_t USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX pairs_u_journal ON pairs_u USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX pairs_k_journal ON pairs_k USING sift_col (account, tenant_id) WITH (track_changes = 'ctid');
VACUUM (FREEZE, ANALYZE) events, var_events, uuid_events, key_events, pairs_t, pairs_u, pairs_k;
CREATE INDEX events_t ON events USING sift_col (id, account, created_at, kind, v) WITH (key_cut = 'created_at', scope = 'tenant_id: account -> pairs_t(account, tenant_id)');
CREATE INDEX events_v ON var_events USING sift_col (id, account, created_at, kind, v) WITH (key_cut = 'created_at', scope = 'tenant_id: account -> pairs_t(account, tenant_id)');
CREATE INDEX events_u ON uuid_events USING sift_col (id, account, created_at, kind, v) WITH (key_cut = 'created_at', scope = 'tenant_id: account -> pairs_u(account, tenant_id)');
CREATE INDEX events_k ON key_events USING sift_col (id, account, created_at, kind, v) WITH (key_cut = 'created_at', scope = 'tenant_id: account -> pairs_k(account, tenant_id)');
SELECT i AS index, count(*) FILTER (WHERE s.format >= 53) > 0 AS sidecar_written_by_the_build FROM (VALUES ('events_t'), ('events_v'), ('events_u'), ('events_k')) v (i), LATERAL sift._test_col_formats(i::regclass) s GROUP BY 1 ORDER BY 1;

-- the keys of the reads: tenants 1 to 7 (7 has no pair until the accounts of 6 are re-keyed) and one nobody has
CREATE TABLE keys (k int PRIMARY KEY);
INSERT INTO keys VALUES (1), (2), (3), (4), (5), (6), (7), (99);

-- the statement of one key over the pairs of a variant: t (text facts and pairs), v (varchar facts, text pairs), u (uuid), k (text member, text key)
CREATE FUNCTION q(variant text, k int) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT kind, count(*) AS n, sum(v) AS s FROM %s e WHERE e.account IN (SELECT account FROM pairs_%s WHERE tenant_id = %s) GROUP BY kind',
                 CASE variant WHEN 'v' THEN 'var_events' WHEN 'u' THEN 'uuid_events' WHEN 'k' THEN 'key_events' ELSE 'events' END, CASE variant WHEN 'v' THEN 't' ELSE variant END,
                 CASE variant WHEN 'k' THEN quote_literal('tenant-' || k) ELSE k::text END) $$;

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	DROP TABLE IF EXISTS side_rows;
	EXECUTE 'CREATE TEMP TABLE side_rows AS ' || q;
	SELECT md5(coalesce(string_agg(t::text, '|' ORDER BY t::text), '')) INTO r FROM side_rows t;
	DROP TABLE side_rows;
	RETURN r;
END $$;

CREATE FUNCTION runs_of(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; r int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope Sidecar Runs' THEN r := regexp_replace(l, '^.*: ', '')::int; END IF;
	END LOOP;
	RETURN r;
END $$;

-- for each variant: the keys whose rows are not the join's with the runs (the entity scan) and without them, the keys that took runs, and the runs taken without being offered them
CREATE FUNCTION check_state(state text) RETURNS TABLE (st text, variant text, wrong_with_runs text, wrong_without_runs text, keys_with_runs text, runs_without_runs int) LANGUAGE plpgsql AS $$
DECLARE variant text; k int; join_md5 text; with_runs int; wrong1 int[]; wrong2 int[]; took int[]; leaked int;
BEGIN
	FOREACH variant IN ARRAY ARRAY['t', 'v', 'u', 'k'] LOOP
		wrong1 := '{}'; wrong2 := '{}'; took := '{}'; leaked := 0;
		FOR k IN SELECT x.k FROM keys x ORDER BY x.k LOOP
			SET LOCAL sift.agg_scope = on;
			SET LOCAL sift.enable_agg = off;
			join_md5 := rows_md5(q(variant, k));
			SET LOCAL sift.enable_agg = on;
			SET LOCAL sift.test_scope_entity = on;
			SET LOCAL sift.test_scope_no_side = off;
			IF rows_md5(q(variant, k)) <> join_md5 THEN wrong1 := wrong1 || k; END IF;
			with_runs := runs_of(q(variant, k));
			IF with_runs > 0 THEN took := took || k; END IF;
			SET LOCAL sift.test_scope_no_side = on;
			IF rows_md5(q(variant, k)) <> join_md5 THEN wrong2 := wrong2 || k; END IF;
			leaked := leaked + runs_of(q(variant, k));
		END LOOP;
		RETURN QUERY SELECT state, variant, coalesce(array_to_string(wrong1, ','), ''), coalesce(array_to_string(wrong2, ','), ''), coalesce(array_to_string(took, ',')), leaked;
	END LOOP;
END $$;

SELECT * FROM check_state('fresh');

-- a read of a key whose members no pair change touched reads the units of its runs and no other; one whose member was added reads every unit, which the sidecar cannot place
CREATE FUNCTION units_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	SET LOCAL sift.agg_scope = on;
	SET LOCAL sift.test_scope_entity = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Scope (Units|Rows Read)' THEN r := r || regexp_replace(l, '^ *Scope ', '') || '; '; END IF;
	END LOOP;
	RETURN rtrim(r, '; ');
END $$;
SELECT 'fresh' AS state, k, units_of(q('t', k)) AS read FROM keys WHERE k IN (2, 3) ORDER BY k;

-- pairs added (an account that has facts, one that has none, one only the facts at the end have), removed, re-keyed, an account renamed
INSERT INTO pairs_t VALUES ('acct-2', 4), ('acct-new', 4), ('acct-61', 4);
INSERT INTO pairs_u SELECT md5(account)::uuid, tenant_id FROM pairs_t WHERE tenant_id = 4 AND account IN ('acct-2', 'acct-new', 'acct-61');
INSERT INTO pairs_k SELECT account, 'tenant-' || tenant_id FROM pairs_t WHERE tenant_id = 4 AND account IN ('acct-2', 'acct-new', 'acct-61');
DELETE FROM pairs_t WHERE tenant_id = 5 AND account ~ '^acct-1[0-9]$';
DELETE FROM pairs_u WHERE tenant_id = 5 AND account IN (SELECT md5('acct-' || i)::uuid FROM generate_series(10, 19) i);
DELETE FROM pairs_k WHERE tenant_id = 'tenant-5' AND account ~ '^acct-1[0-9]$';
UPDATE pairs_t SET tenant_id = 7 WHERE tenant_id = 6 AND account ~ '^acct-[0-9]*[05]$';
UPDATE pairs_u SET tenant_id = 7 WHERE tenant_id = 6 AND account IN (SELECT md5('acct-' || i)::uuid FROM generate_series(1, 60) i WHERE i % 5 = 0);
UPDATE pairs_k SET tenant_id = 'tenant-7' WHERE tenant_id = 'tenant-6' AND account ~ '^acct-[0-9]*[05]$';
UPDATE pairs_t SET account = 'acct-renamed-' || account WHERE tenant_id = 1 AND account ~ '^acct-[0-9]*[59]$';
UPDATE pairs_u SET account = md5('acct-renamed-' || account::text)::uuid WHERE tenant_id = 1 AND account IN (SELECT md5('acct-' || i)::uuid FROM generate_series(1, 60) i WHERE i % 10 IN (5, 9));
UPDATE pairs_k SET account = 'acct-renamed-' || account WHERE tenant_id = 'tenant-1' AND account ~ '^acct-[0-9]*[59]$';
SELECT * FROM check_state('pairs edited');
SELECT 'pairs edited' AS state, k, units_of(q('t', k)) AS read FROM keys WHERE k IN (2, 3, 4) ORDER BY k;

-- the merge writes the sidecars again from the pairs now, with the words of the members the units hold
SELECT sift.col_promote('events_t') IS NOT NULL AS promoted_t, sift.col_promote('events_v') IS NOT NULL AS promoted_v, sift.col_promote('events_u') IS NOT NULL AS promoted_u, sift.col_promote('events_k') IS NOT NULL AS promoted_k;
SELECT sift.col_merge('events_t') IS NOT NULL AS merged_t, sift.col_merge('events_v') IS NOT NULL AS merged_v, sift.col_merge('events_u') IS NOT NULL AS merged_u, sift.col_merge('events_k') IS NOT NULL AS merged_k;
SELECT * FROM check_state('merged');
SELECT sift.col_maintain('events_t') IS NOT NULL AS maintained_t, sift.col_maintain('events_v') IS NOT NULL AS maintained_v, sift.col_maintain('events_u') IS NOT NULL AS maintained_u, sift.col_maintain('events_k') IS NOT NULL AS maintained_k;
SELECT * FROM check_state('maintained');

-- a member that leaves every pair, and a pair of it that comes back
DELETE FROM pairs_t WHERE account = 'acct-7';
DELETE FROM pairs_u WHERE account = md5('acct-7')::uuid;
DELETE FROM pairs_k WHERE account = 'acct-7';
SELECT * FROM check_state('member gone');
INSERT INTO pairs_t VALUES ('acct-7', 3);
INSERT INTO pairs_u VALUES (md5('acct-7')::uuid, 3);
INSERT INTO pairs_k VALUES ('acct-7', 'tenant-3');
SELECT * FROM check_state('member back');

-- new facts of members inside a run, facts deleted and an account moved
INSERT INTO events SELECT 30000 + i, 'acct-' || (1 + i % 60), timestamptz '2025-06-01' + i * interval '17 minutes', i % 7, i FROM generate_series(1, 300) i;
INSERT INTO var_events SELECT * FROM events WHERE id > 30000;
INSERT INTO uuid_events SELECT id, md5(account)::uuid, created_at, kind, v FROM events WHERE id > 30000;
INSERT INTO key_events SELECT * FROM events WHERE id > 30000;
DELETE FROM events WHERE id % 97 = 0;
DELETE FROM var_events WHERE id % 97 = 0;
DELETE FROM uuid_events WHERE id % 97 = 0;
DELETE FROM key_events WHERE id % 97 = 0;
UPDATE events SET account = 'acct-7' WHERE id % 211 = 0;
UPDATE var_events SET account = 'acct-7' WHERE id % 211 = 0;
UPDATE uuid_events SET account = md5('acct-7')::uuid WHERE id % 211 = 0;
UPDATE key_events SET account = 'acct-7' WHERE id % 211 = 0;
SELECT * FROM check_state('facts edited');

VACUUM (FREEZE, ANALYZE) events, var_events, uuid_events, key_events, pairs_t, pairs_u, pairs_k;
SELECT * FROM check_state('vacuumed');

REINDEX INDEX events_t;
REINDEX INDEX events_v;
REINDEX INDEX events_u;
REINDEX INDEX events_k;
VACUUM (FREEZE, ANALYZE) events, var_events, uuid_events, key_events, pairs_t, pairs_u, pairs_k;
SELECT * FROM check_state('reindexed');

-- a key that left every pair has no run that stands for its members: its read is the join's
DELETE FROM pairs_t WHERE tenant_id = 2;
DELETE FROM pairs_u WHERE tenant_id = 2;
DELETE FROM pairs_k WHERE tenant_id = 'tenant-2';
SELECT * FROM check_state('key gone');

SELECT sift.col_verify('events_t', true) IS NOT NULL AS verified_t, sift.col_verify('events_v', true) IS NOT NULL AS verified_v, sift.col_verify('events_u', true) IS NOT NULL AS verified_u, sift.col_verify('events_k', true) IS NOT NULL AS verified_k;

DROP FUNCTION check_state(text), runs_of(text), rows_md5(text), q(text, int), units_of(text);
DROP TABLE keys, events, var_events, uuid_events, key_events, pairs_t, pairs_u, pairs_k;
