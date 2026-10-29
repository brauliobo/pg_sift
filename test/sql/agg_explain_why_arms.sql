-- sift.explain_why names the arm that made the plan. A top-N whose sort keys the scan holds only in part is a Sort over the scan (the first keys come
-- in order, the Sort puts the rest in order): the arm is used, not refused for its cost. A bare count over a word is planned as SiftCount: the aggregate
-- scan, dearer, is the arm that lost.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, ts date NOT NULL, memo text NOT NULL, deleted_at timestamptz, pad text NOT NULL DEFAULT repeat('x', 300)) WITH (autovacuum_enabled = false);
INSERT INTO events (id, ts, memo, deleted_at) SELECT g, date '2024-01-01' + (hashint4(g) & 1023) % 730, (ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + g % 8] || ' ' || (ARRAY['relay', 'cord', 'clamp', 'bolt', 'valve'])[1 + (g * 7) % 5],
	CASE WHEN g % 37 = 0 THEN now() END FROM generate_series(1, 30000) g;
CREATE INDEX events_col ON events USING sift_col (id, ts, deleted_at);
CREATE INDEX events_memo ON events USING sift (memo);
VACUUM (FREEZE, ANALYZE) events;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l; END LOOP;
	RETURN CASE WHEN p ~ 'SiftCount' THEN 'SiftCount' WHEN p ~ 'Top-N' THEN 'SiftAgg top-N' WHEN p ~ 'SiftAgg' THEN 'SiftAgg' ELSE 'normal' END;
END $$;

SELECT q.name, plan_of(q.sql) AS plan, w.arm, w.used, w.slug
FROM (VALUES ('trailing_key', $q$SELECT id, ts, memo FROM events WHERE deleted_at IS NULL ORDER BY ts, memo, id LIMIT 10$q$),
             ('trailing_key_desc_offset', $q$SELECT id, ts, memo FROM events WHERE deleted_at IS NULL ORDER BY ts DESC, memo DESC, id DESC LIMIT 10 OFFSET 5$q$),
             ('count_of_word', $q$SELECT count(*) FROM events WHERE memo @@> 'cable'$q$)) q(name, sql),
     LATERAL sift.explain_why(q.sql) w
ORDER BY 1, 3;

DROP FUNCTION plan_of(text);
DROP TABLE events;
DROP EXTENSION pg_sift;
