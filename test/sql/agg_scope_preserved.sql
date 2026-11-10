-- The scope conjunct of a scan belongs to the scan wherever the statement puts it, as long as it is a filter of the rows the scan reads: the ON clause of a join whose nullable side is the
-- scanned table (a small table of periods joined to the events: the dashboard shape), a sub-select or a CTE of the scoped events, an inner join, a lateral child. A conjunct that is not
-- such a filter stays an ordinary filter of the scan and EXPLAIN says why (Scope Fallback): the ON clause of a join that keeps the scanned rows, a WHERE or an OR over the rows an outer join
-- made of the unmatched ones, an expression around it, a second scope, a key that is not a constant, a setting. The rows are those of the join to the scope relation in every shape.
-- The test form of the conjunct is the marker sift.in_scope(index, key_name, fk, key); the statements are those of an application, not recognised joins.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 3) j ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at date, value_num int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 320, date '2026-01-01' + (i % 300), i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, value_num)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;

-- what a statement does: whether it is a SiftAgg plan, the scope it reads (also in a plain EXPLAIN), why it reads none (or what it left), the units skipped, and the rows against the plain join
CREATE FUNCTION scope_run(q text, plain text, OUT same boolean, OUT scan boolean, OUT label boolean, OUT why text, OUT fewer_units boolean)
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	e jsonb;
	a text;
	b text;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO e;
	EXECUTE 'SELECT string_agg(t::text, ''|'' ORDER BY t::text) FROM (' || q || ') t' INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'SELECT string_agg(t::text, ''|'' ORDER BY t::text) FROM (' || plain || ') t' INTO b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	same := a IS NOT DISTINCT FROM b;
	scan := jsonb_path_exists(p, '$.**."Node Type" ? (@ == "Custom Scan")');
	label := jsonb_path_exists(e, '$.**."Scope Index"');
	why := coalesce(jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}', 'left: ' || (jsonb_path_query_first(p, '$.**."Scope Filter Left"') #>> '{}'));
	fewer_units := coalesce(jsonb_path_query_first(p, '$.**."Scope Units Skipped"')::text::int, 0) > 0;
END $$;

-- k: the tenant; the marker {M} is the conjunct (its plain form is the join to the scope relation); $V is the VALUES list of the two periods
CREATE TABLE shapes (n int, name text, k int, q text);
INSERT INTO shapes
SELECT n, name, k, replace(replace(q, '{M}', 'sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ' || k || ')'),
	'$V', '(VALUES (''a'', daterange(''2026-02-01'', ''2026-10-31'', ''[]'')), (''b'', daterange(''2025-02-01'', ''2025-10-31'', ''[]'')))')
FROM (VALUES
	(1, 'periods left join events of a sub-select', 3, 'WITH periods AS (SELECT * FROM $V v(period, range)),
		ev AS (SELECT e.id, e.value_num, periods.period bp FROM events e JOIN periods ON e.created_at <@ range WHERE {M})
		SELECT p.period, avg(ev.value_num), count(DISTINCT ev.id) FROM periods p LEFT JOIN ev ON p.period = ev.bp GROUP BY 1'),
	(2, 'periods left join events, in ON', 3, 'SELECT p.period, count(e.id), sum(e.value_num) FROM $V p(period, range) LEFT JOIN events e ON e.created_at <@ p.range AND {M} GROUP BY 1'),
	(3, 'events right join periods, in ON', 3, 'SELECT p.period, count(e.id), sum(e.value_num) FROM events e RIGHT JOIN $V p(period, range) ON e.created_at <@ p.range AND {M} GROUP BY 1'),
	(4, 'periods left join events, in WHERE', 3, 'SELECT p.period, count(e.id), sum(e.value_num) FROM $V p(period, range) LEFT JOIN events e ON e.created_at <@ p.range WHERE {M} GROUP BY 1'),
	(5, 'periods inner join events', 3, 'SELECT p.period, count(e.id), sum(e.value_num) FROM events e JOIN $V p(period, range) ON e.created_at <@ p.range WHERE {M} GROUP BY 1'),
	(6, 'lateral child', 3, 'SELECT p.period, l.c FROM (VALUES (''a'', 1), (''b'', 2)) p(period, v) LEFT JOIN LATERAL (SELECT count(*) c FROM events e WHERE e.value_num = p.v AND {M}) l ON true'),
	(7, 'materialized CTE', 3, 'WITH ev AS MATERIALIZED (SELECT e.created_at, count(*) c, sum(e.value_num) s FROM events e WHERE {M} GROUP BY 1)
		SELECT p.period, sum(ev.c), sum(ev.s) FROM $V p(period, range) LEFT JOIN ev ON ev.created_at <@ p.range GROUP BY 1'),
	(8, 'events left join periods, in ON', 3, 'SELECT count(*), count(p.period) FROM events e LEFT JOIN $V p(period, range) ON e.created_at <@ p.range AND {M}'),
	(9, 'events full join, in ON', 3, 'SELECT count(*), count(p.g), count(e.id) FROM events e FULL JOIN (SELECT g FROM generate_series(1, 30) g) p ON e.dim_a_id = p.g AND {M}'),
	(10, 'periods left join events, in WHERE or no event', 3, 'SELECT p.period, count(e.id), count(*) FROM $V p(period, range) LEFT JOIN events e ON e.created_at <@ p.range WHERE {M} OR e.id IS NULL GROUP BY 1'),
	(11, 'inside an expression', 3, 'SELECT count(*) FROM events e WHERE {M} OR e.value_num > 8'),
	(12, 'two scope conjuncts', 3, 'SELECT count(*) FROM events e WHERE {M} AND sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ARRAY[4, 5]::int8[])'),
	(13, 'a key that is not a constant', 3, 'SELECT count(*) FROM events e WHERE sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, 1 + e.value_num % 3)'),
	(14, 'one scope', 3, 'SELECT count(*), sum(e.value_num) FROM events e WHERE {M}')) s(n, name, k, q);

-- the plain form: the join to the scope relation (the periods of the shapes that keep their rows keep them)
CREATE FUNCTION plain_of(q text, k int) RETURNS text LANGUAGE sql AS $$
	SELECT replace(replace(q, 'sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ' || k || ')', 'e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = ' || k || ')'),
		'sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, ARRAY[4, 5]::int8[])', 'e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id IN (4, 5))')
$$;
CREATE FUNCTION phase() RETURNS TABLE (n int, name text, same boolean, scan boolean, label boolean, why text, fewer_units boolean) LANGUAGE sql AS $$
	SELECT s.n, s.name, r.same, r.scan, r.label, r.why, r.fewer_units FROM shapes s, LATERAL scope_run(s.q, plain_of(s.q, s.k)) r ORDER BY s.n
$$;

-- label: the Scope Index line of a plain EXPLAIN; fewer_units: the units of the other tenants were skipped. why: a conjunct that stayed a filter; scan false: the normal plan
-- (a WHERE over the nullable side of an outer join is evaluated after it, the join stays)
SELECT * FROM phase();

-- the setting off: every conjunct is an ordinary filter, said so
SET sift.agg_scope = off;
SELECT n, same, scan, label, why FROM phase() WHERE n IN (1, 2, 5, 14);
RESET sift.agg_scope;

DROP TABLE shapes, events, tenant_scope;
DROP FUNCTION scope_run(text, text), plain_of(text, int), phase();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
