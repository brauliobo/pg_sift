-- Ranked scans under row-level security (sift.rls_scores, off by default). With it on, a role under a policy gets
-- the ranked scan: the policy's quals run on every row the ranker offers before any qual of the query, and the ranker is asked for more
-- rows until k pass. The document frequencies and field lengths are those of the whole index, so a visible row scores as the owner
-- sees it. Every query returns what the superuser's full ranking, filtered by the tenant afterwards, returns (the reference).
CREATE EXTENSION pg_sift;
\set VERBOSITY terse

CREATE ROLE sift_t_rr_user;
CREATE ROLE sift_t_rr_owner;
CREATE ROLE sift_t_rr_bypass BYPASSRLS;
GRANT USAGE ON SCHEMA sift TO sift_t_rr_user, sift_t_rr_owner, sift_t_rr_bypass;
GRANT CREATE ON SCHEMA public TO sift_t_rr_owner;

CREATE FUNCTION rr_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	FOR l IN EXECUTE q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION rr_rows(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'SELECT coalesce(jsonb_agg(to_jsonb(t)), ''[]'') FROM (' || q || ') t' INTO j;
	RETURN j;
END $$;

-- not leakproof, and raising for the rows nobody may see
CREATE FUNCTION rr_boom(t text) RETURNS boolean LANGUAGE plpgsql STABLE AS $$
BEGIN
	IF t = 'hidden' THEN
		RAISE EXCEPTION 'evaluated on a hidden row';
	END IF;
	RETURN true;
END $$;
CREATE FUNCTION rr_mine(t text) RETURNS boolean LANGUAGE sql STABLE AS $$ SELECT t = current_user $$;

CREATE TABLE rr_items (id int, tenant text, body text);
INSERT INTO rr_items SELECT i, (ARRAY['sift_t_rr_user', 'sift_t_rr_owner', 'hidden', 'hidden'])[1 + i % 4],
	repeat('alpha ', 1 + i % 5) || repeat('beta ', (i / 4) % 4) || 'w' || i % 31 FROM generate_series(1, 4000) i;
CREATE INDEX rr_items_body ON rr_items USING sift (body);
CREATE TABLE rr_tags (id int PRIMARY KEY, tag text);
INSERT INTO rr_tags SELECT i, 't' || i FROM generate_series(1, 4000) i WHERE i % 3 <> 0;
CREATE TABLE rr_grants (grantee text);
INSERT INTO rr_grants VALUES ('sift_t_rr_user'), ('sift_t_rr_owner');
VACUUM ANALYZE rr_items, rr_tags, rr_grants;
ALTER TABLE rr_items OWNER TO sift_t_rr_owner;
GRANT SELECT ON rr_items, rr_tags, rr_grants TO sift_t_rr_user, sift_t_rr_bypass;
GRANT EXECUTE ON FUNCTION rr_plan(text), rr_rows(text), rr_boom(text), rr_mine(text) TO sift_t_rr_user, sift_t_rr_owner, sift_t_rr_bypass;
ALTER TABLE rr_items ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON rr_items USING (tenant = current_user);

-- the reference of each query: the rows of a query the superuser runs, who sees every row (the full ranking in a subquery that
-- PostgreSQL does not flatten, the tenant filtered afterwards)
CREATE TABLE rr_ref (k text PRIMARY KEY, rows jsonb);
GRANT SELECT ON rr_ref TO sift_t_rr_user, sift_t_rr_owner, sift_t_rr_bypass;
CREATE FUNCTION rr_set(k text, q text) RETURNS void LANGUAGE sql AS $$ INSERT INTO rr_ref VALUES (k, rr_rows(q)) $$;
-- the query of the role returns the reference's rows; scan: its plan has the scan
CREATE FUNCTION rr_check(k text, q text, OUT n int, OUT same boolean, OUT scan boolean) LANGUAGE plpgsql AS $$
BEGIN
	n := jsonb_array_length(rr_rows(q));
	same := rr_rows(q) = (SELECT rows FROM rr_ref r WHERE r.k = rr_check.k);
	scan := rr_plan('EXPLAIN (COSTS OFF) ' || q) ~ 'SiftScan';
END $$;
GRANT EXECUTE ON FUNCTION rr_check(text, text) TO sift_t_rr_user, sift_t_rr_owner, sift_t_rr_bypass;

CREATE VIEW rr_ranked AS SELECT id, tenant, ctid AS c, sift.score(ctid) AS s FROM rr_items WHERE body @@> 'alpha beta' OFFSET 0;
SELECT rr_set('top10', 'SELECT id, s FROM rr_ranked WHERE tenant = ''sift_t_rr_user'' ORDER BY s DESC, c LIMIT 10');
SELECT rr_set('top3', 'SELECT id, s FROM rr_ranked WHERE tenant = ''sift_t_rr_user'' ORDER BY s DESC, c LIMIT 3');
SELECT rr_set('all', 'SELECT id, s FROM rr_ranked WHERE tenant = ''sift_t_rr_user'' ORDER BY s DESC, c');
SELECT rr_set('page', 'SELECT id, s FROM rr_ranked WHERE tenant = ''sift_t_rr_user'' ORDER BY s DESC, c OFFSET 5 LIMIT 5');
SELECT rr_set('cutoff', 'SELECT id, s FROM rr_ranked WHERE tenant = ''sift_t_rr_user'' AND s > 0.0003 ORDER BY s DESC, c LIMIT 8');
SELECT rr_set('by_id', 'SELECT id, s FROM rr_ranked WHERE tenant = ''sift_t_rr_user'' ORDER BY id LIMIT 6');
SELECT rr_set('join', 'SELECT q.id, t.tag FROM rr_ranked q JOIN rr_tags t USING (id) WHERE q.tenant = ''sift_t_rr_user'' ORDER BY q.s DESC, q.c LIMIT 5');
SELECT rr_set('any', 'SELECT id, s FROM (SELECT id, tenant, ctid AS c, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha'' OFFSET 0) q
	WHERE tenant = ''sift_t_rr_user'' ORDER BY s DESC, c LIMIT 10');
SELECT rr_set('owner_top5', 'SELECT id, s FROM rr_ranked WHERE tenant = ''sift_t_rr_owner'' ORDER BY s DESC, c LIMIT 5');
SELECT rr_set('top5_user', 'SELECT id FROM rr_ranked WHERE tenant = ''sift_t_rr_user'' ORDER BY s DESC, c LIMIT 5');
SELECT rr_set('top5_user_s', 'SELECT id, s FROM rr_ranked WHERE tenant = ''sift_t_rr_user'' ORDER BY s DESC, c LIMIT 5');
SELECT rr_set('top5_all', 'SELECT id, s FROM rr_ranked ORDER BY s DESC, c LIMIT 5');
SELECT rr_set('count', 'SELECT count(*) AS n FROM rr_items WHERE body @@> ''alpha beta'' AND tenant = ''sift_t_rr_user''');

-- 1. off, the default: the role gets no ranked scan, the score raises an error, and a role cannot switch the setting on
SET ROLE sift_t_rr_user;
\set VERBOSITY default
SELECT id, sift.score(ctid) FROM rr_items WHERE body @@> 'alpha beta' ORDER BY sift.score(ctid) DESC LIMIT 3;
\set VERBOSITY terse
SELECT rr_plan('EXPLAIN (COSTS OFF) SELECT id FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY id LIMIT 3') ~ 'Sift' AS unranked_scan;
SET sift.rls_scores = on;
RESET ROLE;

-- 2. on: the rows and the order of the reference
SET sift.rls_scores = on;
SET ROLE sift_t_rr_user;
SET sift.rls_scores = off;
SELECT * FROM rr_check('top10', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 10');
SELECT * FROM rr_check('top3', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 3');
SELECT * FROM rr_check('all', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC');
SELECT * FROM rr_check('page', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC OFFSET 5 LIMIT 5');
SELECT * FROM rr_check('cutoff', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' AND sift.score(ctid) > 0.0003 ORDER BY sift.score(ctid) DESC LIMIT 8');
SELECT * FROM rr_check('by_id', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY id LIMIT 6');
SELECT * FROM rr_check('join', 'SELECT i.id, t.tag FROM rr_items i JOIN rr_tags t USING (id) WHERE i.body @@> ''alpha beta'' ORDER BY sift.score(i.ctid) DESC, i.ctid LIMIT 5');
SELECT * FROM rr_check('any', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ANY (ARRAY[''alpha'']) ORDER BY sift.score(ctid) DESC LIMIT 10');
-- no ranking asked for: the matches come from a sequential scan, as they do without the setting, and the count has no pushdown
SELECT * FROM rr_check('count', 'SELECT count(*) AS n FROM rr_items WHERE body @@> ''alpha beta''');
SELECT rr_plan('EXPLAIN (COSTS OFF) SELECT id FROM rr_items WHERE body @@> ''alpha beta''') ~ 'Sift' AS unranked_scan;
-- max_score is the best of all the matches, the hidden ones too
SELECT sift.max_score(ctid) FROM rr_items WHERE body @@> 'alpha beta' LIMIT 1;
RESET ROLE;

-- 3. the policy runs before the query's quals: a qual that raises for a hidden row, and a function that does, never meet one
SELECT 100 / (id - 3) FROM rr_items WHERE id = 3;
SELECT rr_boom(tenant) FROM rr_items WHERE id = 3;
SET ROLE sift_t_rr_user;
SELECT * FROM rr_check('top5_user', 'SELECT id FROM rr_items WHERE body @@> ''alpha beta'' AND 100 / (id - 3) IS NOT NULL ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT * FROM rr_check('top5_user', 'SELECT id FROM rr_items WHERE body @@> ''alpha beta'' AND rr_boom(tenant) ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT count(*) AS no_row_is_hidden FROM rr_items WHERE body @@> 'alpha' AND tenant <> 'sift_t_rr_user';
SELECT count(*) AS no_hidden_in_ranking FROM (SELECT tenant FROM rr_items WHERE body @@> 'alpha' ORDER BY sift.score(ctid) DESC) q WHERE tenant <> 'sift_t_rr_user';
RESET ROLE;

-- 4. policies that call functions and read other tables
DROP POLICY own ON rr_items;
CREATE POLICY own ON rr_items USING (rr_mine(tenant));
SET ROLE sift_t_rr_user;
-- a dearer policy than the query's quals still runs first
SELECT * FROM rr_check('top5_user', 'SELECT id FROM rr_items WHERE body @@> ''alpha beta'' AND 100 / (id - 3) IS NOT NULL AND rr_boom(tenant) ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT * FROM rr_check('top10', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 10');
RESET ROLE;
DROP POLICY own ON rr_items;
CREATE POLICY own ON rr_items USING (tenant IN (SELECT grantee FROM rr_grants WHERE grantee = current_user));
SET ROLE sift_t_rr_user;
SELECT * FROM rr_check('top5_user', 'SELECT id FROM rr_items WHERE body @@> ''alpha beta'' AND 100 / (id - 3) IS NOT NULL AND rr_boom(tenant) ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT * FROM rr_check('top10', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 10');
SELECT * FROM rr_check('all', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC');
RESET ROLE;
DROP POLICY own ON rr_items;
CREATE POLICY own ON rr_items USING (tenant = current_user);

-- 5. the owner bypasses the policy until FORCE; a role with BYPASSRLS never meets it; both score as the superuser does
SET ROLE sift_t_rr_owner;
SELECT * FROM rr_check('top5_all', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 5');
RESET ROLE;
ALTER TABLE rr_items FORCE ROW LEVEL SECURITY;
SET ROLE sift_t_rr_owner;
SELECT * FROM rr_check('owner_top5', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 5');
RESET ROLE;
ALTER TABLE rr_items NO FORCE ROW LEVEL SECURITY;
SET ROLE sift_t_rr_bypass;
SELECT * FROM rr_check('top5_all', 'SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 5');
RESET ROLE;

-- 6. a prepared statement planned for a role that bypasses the policy is planned again for the one under it
SET ROLE sift_t_rr_bypass;
PREPARE rr_p AS SELECT id, sift.score(ctid) AS s FROM rr_items WHERE body @@> 'alpha beta' ORDER BY sift.score(ctid) DESC LIMIT 5;
EXECUTE rr_p;
RESET ROLE;
SET ROLE sift_t_rr_user;
CREATE TEMP TABLE rr_got AS EXECUTE rr_p;
SELECT (SELECT jsonb_agg(to_jsonb(g)) FROM rr_got g) = (SELECT rows FROM rr_ref WHERE k = 'top5_user_s') AS prepared_applies_the_policy;
RESET ROLE;
DEALLOCATE rr_p;

-- 7. EXPLAIN: the plan and its estimates do not depend on the hidden rows, and ANALYZE does not count them
CREATE TABLE rr_a (id int, tenant text, body text);
CREATE TABLE rr_b (id int, tenant text, body text);
INSERT INTO rr_a SELECT i, 'sift_t_rr_user', 'needle visible ' || i FROM generate_series(1, 20) i;
INSERT INTO rr_b SELECT i, 'sift_t_rr_user', 'needle visible ' || i FROM generate_series(1, 20) i;
INSERT INTO rr_a SELECT i, 'hidden', 'needle needle hidden ' || i FROM generate_series(21, 3020) i;
INSERT INTO rr_b SELECT i, 'hidden', 'zzzzzz yyyyyy hidden ' || i FROM generate_series(21, 3020) i;
CREATE INDEX rr_a_idx ON rr_a USING sift (body);
CREATE INDEX rr_b_idx ON rr_b USING sift (body);
VACUUM ANALYZE rr_a, rr_b;
GRANT SELECT ON rr_a, rr_b TO sift_t_rr_user;
ALTER TABLE rr_a ENABLE ROW LEVEL SECURITY;
ALTER TABLE rr_b ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON rr_a USING (tenant = current_user);
CREATE POLICY own ON rr_b USING (tenant = current_user);
SET sift.rls_scores = on;
SET ROLE sift_t_rr_user;
SELECT rr_plan('EXPLAIN SELECT id FROM rr_a WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC LIMIT 5') ~ 'SiftScan' AS scan_a,
	   rr_plan('EXPLAIN SELECT id FROM rr_b WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC LIMIT 5') ~ 'SiftScan' AS scan_b;
SELECT replace(rr_plan('EXPLAIN SELECT id FROM rr_a WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC LIMIT 5'), 'rr_a', 'rr_x')
	 = replace(rr_plan('EXPLAIN SELECT id FROM rr_b WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC LIMIT 5'), 'rr_b', 'rr_x') AS same_plan_and_estimates;
SELECT replace(rr_plan('EXPLAIN SELECT id FROM rr_a WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC'), 'rr_a', 'rr_x')
	 = replace(rr_plan('EXPLAIN SELECT id FROM rr_b WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC'), 'rr_b', 'rr_x') AS same_plan_and_estimates_drained;
SELECT rr_plan('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM rr_a WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC LIMIT 5')
	~ '(candidates|superblocks|blocks scored|heap checks|index searches)' AS counters_of_the_index_shown;
SELECT rr_plan('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM rr_a WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC LIMIT 5')
	~ 'Order: score DESC' AS order_shown;
RESET ROLE;
SELECT rr_plan('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM rr_a WHERE body @@> ''needle'' ORDER BY sift.score(ctid) DESC LIMIT 5')
	~ 'candidates' AS owner_sees_the_counters;

-- 8. a partitioned table: a ranked scan per partition under the parent's policy, merged (the statistics are those of each partition)
CREATE TABLE rr_pt (id int, tenant text, body text) PARTITION BY RANGE (id);
CREATE TABLE rr_pt1 PARTITION OF rr_pt FOR VALUES FROM (0) TO (1000);
CREATE TABLE rr_pt2 PARTITION OF rr_pt FOR VALUES FROM (1000) TO (2000);
INSERT INTO rr_pt SELECT i, (ARRAY['sift_t_rr_user', 'hidden'])[1 + i % 2], 'alpha ' || repeat('beta ', i % 5) || 'w' || i % 17 FROM generate_series(1, 1999) i;
CREATE INDEX rr_pt1_idx ON rr_pt1 USING sift (body);
CREATE INDEX rr_pt2_idx ON rr_pt2 USING sift (body);
ANALYZE rr_pt;
GRANT SELECT ON rr_pt, rr_pt1, rr_pt2 TO sift_t_rr_user;
ALTER TABLE rr_pt ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON rr_pt USING (tenant = current_user);
SELECT rr_set('part', 'SELECT id, s FROM (SELECT id, tenant, tableoid, ctid AS c, sift.score(ctid) AS s FROM rr_pt WHERE body @@> ''alpha beta'' OFFSET 0) q
	WHERE tenant = ''sift_t_rr_user'' ORDER BY s DESC, tableoid, c LIMIT 7');
SET ROLE sift_t_rr_user;
SELECT * FROM rr_check('part', 'SELECT id, sift.score(ctid) AS s FROM rr_pt WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 7');
RESET ROLE;

-- 9. a policy that is itself a match: the rows it keeps come from the heap, and its word is not a term of the ranking (the scores are the owner's)
CREATE TABLE rr_pm (id int, tenant text, body text);
INSERT INTO rr_pm SELECT i, (ARRAY['sift_t_rr_user', 'hidden'])[1 + i % 2], CASE WHEN i % 4 < 2 THEN 'open ' ELSE 'closed ' END || repeat('beta ', 1 + (i / 4) % 3) || 'w' || i % 7
	FROM generate_series(1, 1200) i;
CREATE INDEX rr_pm_body ON rr_pm USING sift (body);
ANALYZE rr_pm;
GRANT SELECT ON rr_pm TO sift_t_rr_user;
ALTER TABLE rr_pm ENABLE ROW LEVEL SECURITY;
CREATE POLICY own ON rr_pm USING (tenant = current_user AND body @@> 'open');
SELECT rr_set('pm', 'SELECT id, s FROM (SELECT id, tenant, body, ctid AS c, sift.score(ctid) AS s FROM rr_pm WHERE body @@> ''beta'' OFFSET 0) q
	WHERE tenant = ''sift_t_rr_user'' AND body ~ ''open'' ORDER BY s DESC, c LIMIT 6');
SET ROLE sift_t_rr_user;
SELECT * FROM rr_check('pm', 'SELECT id, sift.score(ctid) AS s FROM rr_pm WHERE body @@> ''beta'' ORDER BY sift.score(ctid) DESC LIMIT 6');
SELECT rr_plan('EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) AS s FROM rr_pm WHERE body @@> ''beta'' ORDER BY sift.score(ctid) DESC LIMIT 6') ~ 'Query: ''beta''::text\n' AS query_is_the_users;
RESET ROLE;

RESET sift.rls_scores;

DROP VIEW rr_ranked;
DROP FUNCTION rr_check(text, text), rr_set(text, text), rr_rows(text), rr_plan(text), rr_boom(text), rr_mine(text);
DROP TABLE rr_items, rr_tags, rr_grants, rr_ref, rr_a, rr_b, rr_pt, rr_pm;
DROP OWNED BY sift_t_rr_user, sift_t_rr_owner, sift_t_rr_bypass;
DROP ROLE sift_t_rr_user, sift_t_rr_owner, sift_t_rr_bypass;
DROP EXTENSION pg_sift;
