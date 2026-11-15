-- The automatic key_cut leaves a key alone when one value holds more rows than a segment holds (the rows whose arrays fit array_bytes, at most 2^26): the rows of
-- that value are runs the rewrite has to order, not the units of a key. The statistics say so (the first of the most common values); the NOTICE and
-- sift.key_cut_decision name the value, for a key_cut of its own and for the group of a key_order that implies one. A key_cut named by the user is built whatever its values.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE hv (id int PRIMARY KEY, tenant_id int, grp_id int, day date, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO hv SELECT i, 1 + i % 101, CASE WHEN i % 10 < 3 THEN 1 ELSE 2 + i % 101 END, DATE '2010-01-01' + (i * 7919) % 5000, repeat('x', 100) FROM generate_series(1, 60000) i;
VACUUM (FREEZE, ANALYZE) hv;
SELECT attname, round(max(f)::numeric, 2) AS hottest FROM pg_stats s, unnest(s.most_common_freqs) f WHERE tablename = 'hv' AND attname = 'grp_id' GROUP BY 1 ORDER BY 1;

-- CREATE INDEX stores the count of rows it saw in reltuples (PostgreSQL 17 does): the estimate of a large table is set again before the build and before the decision
CREATE TEMP TABLE hv_est (n bigint);
-- sets the estimate and checks that the planner, which reads the relcache entry the build and the decision read, sees it: an estimate that did not arrive fails here, not as another decision
CREATE FUNCTION hv_set_est() RETURNS void LANGUAGE plpgsql AS
$$ DECLARE want bigint := (SELECT max(n) FROM hv_est); plan json; BEGIN
	IF want IS NULL THEN RETURN; END IF;
	UPDATE pg_class SET reltuples = want WHERE relname = 'hv';
	EXECUTE 'EXPLAIN (FORMAT JSON) SELECT * FROM hv' INTO plan;
	IF abs((plan->0->'Plan'->>'Plan Rows')::bigint - want) > want / 100 THEN
		RAISE EXCEPTION 'the planner sees % rows of hv, % were set', plan->0->'Plan'->>'Plan Rows', want;
	END IF;
END $$;
-- the cut the build wrote, and what sift.key_cut_decision says of it under the segment bound given
CREATE FUNCTION hv_cut(cols text, opts text, array_kb int DEFAULT NULL) RETURNS TABLE (built text, decision text, why text) LANGUAGE plpgsql AS
$$ BEGIN
	PERFORM hv_set_est();
	EXECUTE format('CREATE INDEX hv_i ON hv USING sift_col (%s) WITH (%s)', cols, opts);
	PERFORM hv_set_est();
	IF array_kb IS NOT NULL THEN
		PERFORM set_config('sift.test_limits', 'array_bytes=' || array_kb * 1024, true);
	END IF;
	RETURN QUERY SELECT coalesce((SELECT substring(o FROM 'key_cut=(.*)') FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'hv_i' AND o ~ '^key_cut'), 'none'),
		d.decision, regexp_replace(left(d.reason, 100), '[0-9]+', 'N', 'g') FROM sift.key_cut_decision('hv_i') d;
	DROP INDEX hv_i;
END $$;

SELECT * FROM hv_cut('id, tenant_id', 'key_cut = auto');
SELECT * FROM hv_cut('id, grp_id', 'key_cut = auto');
-- a segment of 10,000 rows: the 18,000 rows of group 1 are over it, the 600 of a tenant are not
SELECT * FROM hv_cut('id, grp_id', 'key_cut = auto', 480);
SELECT * FROM hv_cut('id, tenant_id', 'key_cut = auto', 480);
-- the same value of a table of 257 million rows (77 million) is over the segment of the library (2^26 rows)
INSERT INTO hv_est VALUES (257000000);
SELECT * FROM hv_cut('id, grp_id', 'key_cut = auto');
SELECT * FROM hv_cut('id, tenant_id', 'key_cut = auto');
-- the build says it (the NOTICE and sift.index_choices), for the group of a key_order that would imply a cut as for a key of its own
CREATE FUNCTION hv_say(cols text, opts text) RETURNS TABLE (choice text, value text) LANGUAGE plpgsql AS
$$ BEGIN
	PERFORM hv_set_est();
	EXECUTE format('CREATE INDEX hv_i ON hv USING sift_col (%s) WITH (%s)', cols, opts);
	PERFORM hv_set_est();
	RETURN QUERY SELECT c.choice, regexp_replace(c.value, '[0-9]+', 'N', 'g') FROM sift.index_choices('hv_i') c WHERE c.choice = 'key_cut';
	DROP INDEX hv_i;
END $$;
SELECT * FROM hv_say('id, grp_id', 'key_cut = auto');
SELECT * FROM hv_say('id, grp_id, day', 'key_order = ''grp_id, day''');
-- a key_cut of the user's is built whatever its values
SELECT * FROM hv_cut('id, grp_id, day', 'key_order = ''grp_id, day''');
SELECT * FROM hv_cut('id, grp_id, day', 'key_cut = ''grp_id''');

DROP TABLE hv;
DROP FUNCTION hv_cut(text, text, int), hv_say(text, text), hv_set_est();
DROP EXTENSION pg_sift;
