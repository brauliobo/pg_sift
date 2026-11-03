-- A CTE the statement writes MATERIALIZED and several aggregates read is planned inline when the scans cost less than the work of the shared plan, as one written
-- without it is: the notes of each event are read once (jsonb_to_recordset, mapped through a table of codes) and grouped three ways, per event, per group and as a total.
-- The plan must read the events through the scan and not through a copy of the notes; the rows must be the same with the scan on and off for each scope, before and
-- after updates, deletes and a vacuum.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tenants (id int PRIMARY KEY, name text NOT NULL);
INSERT INTO tenants SELECT g, 'tenant ' || g FROM generate_series(1, 6) g;
CREATE TABLE dim_a (id int PRIMARY KEY, label_a text NOT NULL UNIQUE, group_key int NOT NULL);
INSERT INTO dim_a SELECT g, 'code_' || g, 1 + g % 5 FROM generate_series(1, 40) g;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL REFERENCES tenants, event_date date NOT NULL, payload jsonb NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + g % 6, date '2025-01-01' + g % 90,
       jsonb_build_object('notes', (SELECT jsonb_agg(jsonb_build_object('code', 'code_' || (1 + (g * k) % 40), 'score', 1 + (g + k) % 9)) FROM generate_series(1, 1 + g % 4) k))
FROM generate_series(1, 3000) g;
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, event_date, payload);
VACUUM (FREEZE, ANALYZE) events, dim_a, tenants;

CREATE FUNCTION notes_query(scope text) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH mapped_note_rows AS MATERIALIZED (
		SELECT e.id AS event_id, e.tenant_id, m.group_key, n.score
		FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.payload -> 'notes') AS n(code text, score int) JOIN dim_a m ON m.label_a = n.code
		WHERE %s),
	per_event AS (SELECT event_id, group_key, sum(score) AS total FROM mapped_note_rows GROUP BY 1, 2),
	per_group AS (SELECT group_key, count(DISTINCT event_id) AS events, sum(total) AS total FROM per_event GROUP BY 1),
	all_notes AS (SELECT count(*) AS notes, sum(score) AS total FROM mapped_note_rows)
	SELECT group_key::text AS part, events AS n, total FROM per_group UNION ALL SELECT 'all', notes, total FROM all_notes ORDER BY 1$q$, scope)
$$;

-- the CTE scans of the plan (a read of the shared notes), and the scans that read the index
CREATE FUNCTION plan_reads(q text, scan_on bool) RETURNS TABLE (cte_scans int, scans int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	cte_scans := 0; scans := 0;
	PERFORM set_config('sift.enable_agg', scan_on::text, true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		cte_scans := cte_scans + (l ~ 'CTE Scan on mapped_note_rows')::int;
		scans := scans + (l LIKE '%Custom Scan (SiftAgg)%')::int;
	END LOOP;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN NEXT;
END $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $f$
DECLARE a text; b text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT string_agg(r::text, $$|$$ ORDER BY r::text) FROM (%s) r', q) INTO a;
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE format('SELECT string_agg(r::text, $$|$$ ORDER BY r::text) FROM (%s) r', q) INTO b;
	RETURN a IS NOT DISTINCT FROM b AND a IS NOT NULL;
END $f$;

CREATE TABLE scopes (label text PRIMARY KEY, cond text NOT NULL);
INSERT INTO scopes VALUES ('one tenant', 'e.tenant_id = 3'), ('two tenants', 'e.tenant_id IN (2, 5)'), ('no tenant', 'e.tenant_id = 99'), ('a month', 'e.event_date >= date $$2025-02-01$$ AND e.event_date < date $$2025-03-01$$'),
       ('every event', 'true');

-- the scan on: scans read the events and no read of a shared copy; the scan off: the shared CTE
SELECT s.label, r.cte_scans, r.scans > 0 AS scanned FROM scopes s, LATERAL plan_reads(notes_query(s.cond), true) r WHERE s.label IN ('one tenant', 'two tenants', 'a month') ORDER BY s.label;
SELECT s.label, r.cte_scans > 0 AS shared_copy, r.scans FROM scopes s, LATERAL plan_reads(notes_query(s.cond), false) r WHERE s.label = 'one tenant';

SELECT label, same(notes_query(cond)) AS same_rows FROM scopes ORDER BY label;
UPDATE events SET payload = payload - 'notes' WHERE id % 37 = 0;
UPDATE events SET payload = jsonb_set(payload, '{notes,0,score}', 'null') WHERE id % 23 = 0 AND payload ? 'notes';
UPDATE events SET event_date = event_date + 20, tenant_id = 1 + tenant_id % 6 WHERE id % 13 = 0;
SELECT label, same(notes_query(cond)) AS same_rows_updated FROM scopes ORDER BY label;
DELETE FROM events WHERE id % 11 = 0;
SELECT label, same(notes_query(cond)) AS same_rows_deleted FROM scopes ORDER BY label;
VACUUM events;
SELECT label, same(notes_query(cond)) AS same_rows_vacuumed FROM scopes ORDER BY label;
SELECT s.label, r.cte_scans, r.scans > 0 AS scanned FROM scopes s, LATERAL plan_reads(notes_query(s.cond), true) r WHERE s.label IN ('one tenant', 'two tenants') ORDER BY s.label;

DROP FUNCTION same(text);
DROP FUNCTION plan_reads(text, bool);
DROP FUNCTION notes_query(text);
DROP TABLE scopes, events, dim_a, tenants;
DROP EXTENSION pg_sift;
