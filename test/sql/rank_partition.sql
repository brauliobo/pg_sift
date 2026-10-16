-- Ranked top-k through a partitioned parent: each partition runs its own ranked SiftScan with its own index
-- statistics and a MergeAppend takes the global top k. The reference is the same query on every leaf partition, merged by score.
CREATE EXTENSION pg_sift;

CREATE FUNCTION rp_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		-- PostgreSQL 19 names the scan a pruned Result replaces
		CONTINUE WHEN l ~ '^ *Replaces:';
		r := r || l || E'\n';
	END LOOP;
	RETURN rtrim(r, E'\n');
END $$;

-- id:score in delivery order of the top k through the parent, ties by id; the GUCs are set for the statement
CREATE FUNCTION rp_top(parent regclass, q text, flt text, k int, par text DEFAULT 'off', push text DEFAULT 'off') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM set_config('sift.parallel_scan', par, true), set_config('sift.filter_pushdown', push, true);
	EXECUTE format('SELECT string_agg(id || '':'' || s, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, id, s
		FROM (SELECT id, sift.score(ctid) AS s FROM %s WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC, id LIMIT %s) x) y',
		parent, q, flt, k) INTO r;
	RETURN r;
END $$;

-- the scores in delivery order of the top k through the parent, no tie-breaker
CREATE FUNCTION rp_scores(parent regclass, q text, flt text, k int, par text DEFAULT 'off', push text DEFAULT 'off') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM set_config('sift.parallel_scan', par, true), set_config('sift.filter_pushdown', push, true);
	EXECUTE format('SELECT string_agg(s::text, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, s
		FROM (SELECT sift.score(ctid) AS s FROM %s WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC LIMIT %s) x) y',
		parent, q, flt, k) INTO r;
	RETURN r;
END $$;

-- the same, from the leaf partitions queried one by one
CREATE FUNCTION rp_ref(parent regclass, q text, flt text, k int, scores_only bool DEFAULT false) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM set_config('sift.parallel_scan', 'off', true), set_config('sift.filter_pushdown', 'off', true);
	EXECUTE (SELECT format('SELECT string_agg(%s, '','' ORDER BY s DESC, id) FROM (SELECT id, s FROM (%s) u ORDER BY s DESC, id LIMIT %s) t',
			CASE WHEN scores_only THEN 's::text' ELSE 'id || '':'' || s' END,
			string_agg(format('(SELECT id, sift.score(ctid) AS s FROM %s WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC, id LIMIT %s)',
							  relid, q, flt, k), ' UNION ALL '), k)
		FROM pg_partition_tree(parent) WHERE isleaf) INTO r;
	RETURN r;
END $$;

-- ok, or the k whose answer through the parent differs from the reference
CREATE FUNCTION rp_check(parent regclass, q text, flt text DEFAULT 'true', par text DEFAULT 'off', push text DEFAULT 'off') RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	k int;
	bad text := '';
BEGIN
	FOREACH k IN ARRAY ARRAY[1, 5, 50, 100000] LOOP
		IF rp_top(parent, q, flt, k, par, push) IS DISTINCT FROM rp_ref(parent, q, flt, k) THEN
			bad := bad || ' top' || k;
		END IF;
		IF rp_scores(parent, q, flt, k, par, push) IS DISTINCT FROM rp_ref(parent, q, flt, k, true) THEN
			bad := bad || ' metrics_a' || k;
		END IF;
	END LOOP;
	RETURN CASE WHEN bad = '' THEN 'ok' ELSE bad END;
END $$;

-- four partitions of different sizes, so their index statistics (and the scores of equal documents) differ; the last is empty
CREATE TABLE rp (id int, tenant int, body text) PARTITION BY RANGE (id);
CREATE TABLE rp1 PARTITION OF rp FOR VALUES FROM (0) TO (1000);
CREATE TABLE rp2 PARTITION OF rp FOR VALUES FROM (1000) TO (1500);
CREATE TABLE rp3 PARTITION OF rp FOR VALUES FROM (1500) TO (4000);
CREATE TABLE rp4 PARTITION OF rp FOR VALUES FROM (4000) TO (5000);
INSERT INTO rp SELECT i, i % 40, 'alpha ' || repeat('beta ', i % 7) || 'w' || i % 13 || CASE WHEN i % 11 = 0 THEN ' rare' ELSE '' END
FROM generate_series(0, 3999) i;
CREATE INDEX rp_body ON rp USING sift (body);
CREATE INDEX rp_tenant ON rp (tenant);
ANALYZE rp;

-- the plan: a ranked scan of every partition under a MergeAppend; the score reaches the output
SELECT rp_plan('SELECT id, sift.score(ctid) FROM rp WHERE body @@> ''alpha beta'' ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT rp_plan('SELECT id FROM rp WHERE body @@> ''rare'' ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT id, sift.score(ctid) FROM rp WHERE body @@> 'alpha beta' ORDER BY sift.score(ctid) DESC LIMIT 5;
SELECT id, sift.score(ctid) FROM rp WHERE body @@> 'rare OR beta^2' ORDER BY sift.score(ctid) DESC, id LIMIT 5 OFFSET 3;
SELECT id FROM rp WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC, id LIMIT 4;

-- the answer is the reference's: delivery order, ids and scores, for several k
SELECT rp_check('rp', 'alpha beta') AS and_query, rp_check('rp', 'rare') AS rare, rp_check('rp', 'rare OR beta^2') AS or_boost,
	rp_check('rp', 'beta NOT w3') AS not_query, rp_check('rp', '"alpha beta"') AS phrase, rp_check('rp', 'w1*') AS prefix,
	rp_check('rp', 'nothing') AS no_match, rp_check('rp', 'rare', 'tenant = 7') AS filtered;

-- pruning to one partition (a plain ranked SiftScan) and to none
SELECT rp_plan('SELECT id FROM rp WHERE body @@> ''rare'' AND id < 1000 ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT rp_plan('SELECT id FROM rp WHERE body @@> ''rare'' AND id >= 1000 AND id < 2000 ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT rp_plan('SELECT id FROM rp WHERE body @@> ''rare'' AND id < 0 ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT rp_check('rp', 'rare', 'id < 1000') AS one_partition, rp_check('rp', 'rare', 'id >= 1000 AND id < 2000') AS two_partitions,
	rp_check('rp', 'rare', 'id >= 4000') AS empty_partition, rp_check('rp', 'rare', 'id < 0') AS none;
SELECT id, sift.score(ctid) FROM rp WHERE body @@> 'rare' AND id >= 4000 ORDER BY sift.score(ctid) DESC LIMIT 5;
SELECT id, sift.score(ctid) FROM rp WHERE body @@> 'rare' AND id < 0 ORDER BY sift.score(ctid) DESC LIMIT 5;
-- the one partition's scores are the partition's own
SELECT (SELECT array_agg(s) FROM (SELECT sift.score(ctid) s FROM rp2 WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC LIMIT 5) a) =
	(SELECT array_agg(s) FROM (SELECT sift.score(ctid) s FROM rp WHERE body @@> 'rare' AND id >= 1000 AND id < 1500 ORDER BY sift.score(ctid) DESC LIMIT 5) b) AS own_scores;

-- no LIMIT, the score in the select list only, in a subquery and with highlight
SELECT count(*), min(s), max(s) FROM (SELECT sift.score(ctid) AS s FROM rp WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC) t;
SELECT id, sift.score(ctid) FROM rp WHERE body @@> 'rare' AND id BETWEEN 1000 AND 1100 LIMIT 3;
SELECT id, s FROM (SELECT id, sift.score(ctid) AS s FROM rp WHERE body @@> 'rare' ORDER BY s DESC LIMIT 100) t WHERE s > 1.2 ORDER BY s DESC, id LIMIT 3;
SELECT id, sift.score(ctid), sift.highlight(body, 'rare OR beta', '<b>', '</b>') FROM rp WHERE body @@> 'rare OR beta' ORDER BY sift.score(ctid) DESC, id LIMIT 3;

-- the score is read at the query level that scans the partitioned table: a subquery feeds a join or an aggregate
SELECT count(*), round(sum(s)::numeric, 3) FROM (SELECT sift.score(ctid) AS s FROM rp WHERE body @@> 'rare' OFFSET 0) t;
SELECT t.id, t.s, p.tenant FROM (SELECT id, sift.score(ctid) AS s FROM rp WHERE body @@> 'rare' ORDER BY s DESC, id LIMIT 3) t
	JOIN rp p ON p.id = t.id ORDER BY t.s DESC, t.id;
-- an aggregate or a join over the parent reads the scores the Append carries from its partitions' scans (more shapes in rank_join)
SELECT round(sum(sift.score(ctid))::numeric, 3) = (SELECT round(sum(s)::numeric, 3) FROM (SELECT sift.score(ctid) AS s FROM rp
	WHERE body @@> 'rare' OFFSET 0) t) AS same_sum FROM rp WHERE body @@> 'rare';
CREATE TABLE rj (id int PRIMARY KEY, tag text);
INSERT INTO rj SELECT i, 't' || i % 3 FROM generate_series(0, 3999) i;
ANALYZE rj;
SET enable_nestloop = off;
SET enable_mergejoin = off;
SELECT count(*), round(sum(sift.score(rp.ctid))::numeric, 3) FROM rp JOIN rj ON rj.id = rp.id WHERE rp.body @@> 'rare' AND rj.tag = 't1';
SELECT count(*), round(sum(t.s)::numeric, 3) FROM (SELECT id, sift.score(ctid) AS s FROM rp WHERE body @@> 'rare' OFFSET 0) t
	JOIN rj ON rj.id = t.id WHERE rj.tag = 't1';
RESET enable_nestloop;
RESET enable_mergejoin;
DROP TABLE rj;
-- the empty partition (and an empty table) in an unranked scan that reads the score
SELECT count(*) FROM (SELECT id, sift.score(ctid) FROM rp WHERE body @@> 'rare' AND id >= 3900 LIMIT 1) t;
SELECT id, sift.score(ctid) FROM rp WHERE body @@> 'rare' AND id >= 4000 LIMIT 1;
CREATE TABLE re (id int, body text);
CREATE INDEX re_body ON re USING sift (body);
ANALYZE re;
SELECT id, sift.score(ctid) FROM re WHERE body @@> 'rare' LIMIT 1;
SELECT id, sift.score(ctid) FROM re WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC LIMIT 1;
DROP TABLE re;

-- sift.score() outside a ranked scan raises its usual error, and sift.max_score() is refused through the parent
SELECT sift.score(ctid) FROM rp;
SELECT sift.score(ctid) FROM rp WHERE id = 5;
SELECT id, sift.max_score(ctid) FROM rp WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id, sift.max_score(ctid) FROM rp WHERE body @@> 'rare' AND id < 1000 LIMIT 3;
SELECT round(max(sift.max_score(ctid))::numeric, 4) AS partition_max FROM rp2 WHERE body @@> 'rare';

-- filter pushdown and parallel scans: every combination answers what both off answer
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT par, push, rp_check('rp', 'rare OR beta^2', 'tenant = 7', par, push) AS filtered, rp_check('rp', 'alpha beta', 'true', par, push) AS plain
FROM (VALUES ('off', 'off'), ('off', 'on'), ('off', 'force'), ('on', 'off'), ('on', 'on'), ('on', 'force')) m(par, push);
SET sift.filter_pushdown = force;
SELECT rp_plan('SELECT id FROM rp WHERE body @@> ''rare'' AND tenant = 7 ORDER BY sift.score(ctid) DESC LIMIT 5') ~ 'Bitmap Index Scan on rp1_tenant_idx' AS pushed_down;
RESET sift.filter_pushdown;
SET sift.parallel_scan = on;
SELECT id, sift.score(ctid) FROM rp WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
RESET sift.parallel_scan;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_index_scan_size;
RESET max_parallel_workers_per_gather;

-- the same data in one table and in one partition of the parent (the others empty): same ids, scores and tie order
CREATE TABLE rs (id int, body text);
CREATE TABLE ro (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE ro1 PARTITION OF ro FOR VALUES FROM (0) TO (10000);
CREATE TABLE ro2 PARTITION OF ro FOR VALUES FROM (10000) TO (20000);
CREATE TABLE ro3 PARTITION OF ro DEFAULT;
INSERT INTO rs SELECT i, 'alpha ' || repeat('beta ', i % 7) || 'w' || i % 13 || CASE WHEN i % 11 = 0 THEN ' rare' ELSE '' END FROM generate_series(0, 2999) i;
INSERT INTO ro SELECT * FROM rs;
CREATE INDEX rs_body ON rs USING sift (body);
CREATE INDEX ro_body ON ro USING sift (body);
ANALYZE rs;
ANALYZE ro;
CREATE FUNCTION ro_same(q text, k int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	EXECUTE format('SELECT string_agg(id || '':'' || s, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, id, s FROM
		(SELECT id, sift.score(ctid) AS s FROM rs WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT %s) x) y', q, k) INTO a;
	EXECUTE format('SELECT string_agg(id || '':'' || s, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, id, s FROM
		(SELECT id, sift.score(ctid) AS s FROM ro WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT %s) x) y', q, k) INTO b;
	RETURN a IS NOT DISTINCT FROM b AND a IS NOT NULL;
END $$;
SELECT ro_same('alpha beta', 7), ro_same('alpha beta', 1000), ro_same('rare OR beta^2', 40), ro_same('beta NOT w3', 20), ro_same('"alpha beta"', 15);
SELECT rp_plan('SELECT id FROM ro WHERE body @@> ''rare'' ORDER BY sift.score(ctid) DESC LIMIT 5');
DROP TABLE rs, ro;

-- several levels: a sub-partitioned partition and a leaf next to it
CREATE TABLE rm (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE rm_lo PARTITION OF rm FOR VALUES FROM (0) TO (1000) PARTITION BY HASH (id);
CREATE TABLE rm_lo0 PARTITION OF rm_lo FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE rm_lo1 PARTITION OF rm_lo FOR VALUES WITH (MODULUS 2, REMAINDER 1);
CREATE TABLE rm_hi PARTITION OF rm FOR VALUES FROM (1000) TO (3000) PARTITION BY LIST ((id % 2));
CREATE TABLE rm_hi0 PARTITION OF rm_hi FOR VALUES IN (0);
CREATE TABLE rm_hi1 PARTITION OF rm_hi FOR VALUES IN (1);
INSERT INTO rm SELECT i, 'alpha ' || repeat('beta ', i % 7) || 'w' || i % 13 || CASE WHEN i % 11 = 0 THEN ' rare' ELSE '' END
FROM generate_series(0, 2999) i;
CREATE INDEX rm_body ON rm USING sift (body);
ANALYZE rm;
SELECT rp_plan('SELECT id FROM rm WHERE body @@> ''rare'' ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT rp_check('rm', 'rare') AS rare, rp_check('rm', 'alpha beta') AS and_query, rp_check('rm', 'rare OR beta^2', 'id < 1500') AS pruned,
	rp_check('rm', 'rare', 'id < 500') AS one_level;
SELECT rp_plan('SELECT id FROM rm WHERE body @@> ''rare'' AND id >= 1000 AND id % 2 = 1 ORDER BY sift.score(ctid) DESC LIMIT 5');
SELECT id, sift.score(ctid) FROM rm WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT id, sift.max_score(ctid) FROM rm WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC LIMIT 3;
DROP TABLE rm;

-- inheritance parents rank the same way
CREATE TABLE ri (id int, body text);
CREATE TABLE ri1 () INHERITS (ri);
CREATE TABLE ri2 () INHERITS (ri);
INSERT INTO ri1 SELECT i, 'alpha ' || repeat('beta ', i % 7) || CASE WHEN i % 11 = 0 THEN ' rare' ELSE '' END FROM generate_series(0, 999) i;
INSERT INTO ri2 SELECT i + 1000, 'alpha ' || repeat('beta ', i % 5) || CASE WHEN i % 7 = 0 THEN ' rare' ELSE '' END FROM generate_series(0, 299) i;
CREATE INDEX ri_body ON ri USING sift (body);
CREATE INDEX ri1_body ON ri1 USING sift (body);
CREATE INDEX ri2_body ON ri2 USING sift (body);
ANALYZE ri;
SELECT id, sift.score(ctid) FROM ri WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
DROP TABLE ri CASCADE;

-- a partition without a sift index: matching and count(*) work, ranking says what is missing
CREATE TABLE rx (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE rx1 PARTITION OF rx FOR VALUES FROM (0) TO (100);
CREATE TABLE rx2 PARTITION OF rx FOR VALUES FROM (100) TO (200);
CREATE TABLE rx3 PARTITION OF rx FOR VALUES FROM (200) TO (300);
INSERT INTO rx SELECT i, 'alpha ' || repeat('beta ', i % 5) FROM generate_series(0, 299) i;
CREATE INDEX rx1_body ON rx1 USING sift (body);
CREATE INDEX rx2_body ON rx2 USING sift (body);
ANALYZE rx;
SELECT count(*) FROM rx WHERE body @@> 'beta';
SELECT count(*) FROM (SELECT id FROM rx WHERE body @@> 'beta' OFFSET 0) t;
SELECT id, sift.score(ctid) FROM rx WHERE body @@> 'beta' ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id, sift.score(ctid) FROM rx WHERE body @@> 'beta' AND id < 200 ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id, sift.score(ctid) FROM rx WHERE body @@> 'beta' AND id >= 200 ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT id, sift.score(ctid) FROM rx WHERE id < 200 ORDER BY id LIMIT 1;
DROP TABLE rx;

DROP TABLE rp;
DROP FUNCTION rp_plan(text), rp_top(regclass, text, text, int, text, text), rp_scores(regclass, text, text, int, text, text),
	rp_ref(regclass, text, text, int, bool), rp_check(regclass, text, text, text, text), ro_same(text, int);
DROP EXTENSION pg_sift;
