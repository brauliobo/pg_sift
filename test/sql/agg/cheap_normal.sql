-- A statement whose normal plan costs under half of the least the scan costs (a point lookup, a keyset page) is planned by PostgreSQL alone: no arm of the scan
-- is priced. The plan and the rows must be those of the arms, a statement the normal plan does not make cheap must keep the scan, and sift.explain_why
-- still plans every arm.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE cn (id int PRIMARY KEY, a int, b int, ts timestamp);
INSERT INTO cn SELECT i, i % 50, i % 7, timestamp '2024-01-01' + i * interval '1 minute' FROM generate_series(1, 20000) i;
CREATE INDEX cn_col ON cn USING sift_col (a, b, ts);
CREATE INDEX cn_ts ON cn (ts, id);
VACUUM (FREEZE, ANALYZE) cn;

CREATE TABLE cn_q (tag text PRIMARY KEY, q text NOT NULL);
INSERT INTO cn_q VALUES
	('point_sum',   $q$SELECT sum(b), count(*) FROM cn WHERE id = 4242$q$),
	('keyset',      $q$SELECT id, ts FROM cn WHERE (ts, id) < (timestamp '2024-01-10', 20000) ORDER BY ts DESC, id DESC LIMIT 20$q$),
	('first_rows',  $q$SELECT id FROM cn ORDER BY ts LIMIT 5$q$),
	('group_all',   $q$SELECT a, count(*), sum(b) FROM cn GROUP BY a$q$),
	('group_cut',   $q$SELECT b, count(*) FROM cn WHERE ts >= timestamp '2024-01-05' GROUP BY b$q$);

CREATE FUNCTION cn_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text; plan text := '';
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || ' / '; END LOOP;
	RETURN CASE WHEN plan ~ 'SiftAgg' THEN 'scan' ELSE 'normal' END;
END $$;

SELECT tag, cn_plan(q) AS plan FROM cn_q ORDER BY tag COLLATE "C";

-- the rows of the plan by default are those of the scan off
CREATE FUNCTION cn_rows(q text, pushed bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO r;
	RETURN md5(r);
END $$;
SELECT tag, cn_rows(q, true) = cn_rows(q, false) AS same FROM cn_q ORDER BY tag COLLATE "C";

-- explain_why still plans the arms of the statement and names the one that refused it
SELECT arm, used, slug FROM sift.explain_why($$SELECT sum(b), count(*) FROM cn WHERE id = 4242$$) ORDER BY arm;

DROP FUNCTION cn_plan(text), cn_rows(text, bool);
DROP TABLE cn_q, cn;
DROP EXTENSION pg_sift;
