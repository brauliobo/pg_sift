-- The tables of the plan-choice gate (scripts/plangate.sh, test/plan/shapes.sql): events (the fact rows, :n of them), dims (the parents, one per
-- 500 events, at least 60), facts (a second, smaller fact table), a month spine, spans and a partitioned copy of the events. Every draw is a
-- hash of (row number, seed): the same arguments give the same rows, and ANALYZE reads every row (statistics target 1000), so the planner sees
-- the same statistics on every run.
--
--   psql -v n=30000 -v seed=1 -f gen.sql
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 30000
\endif
\if :{?seed}
\else
\set seed 1
\endif
SET default_statistics_target = 1000;
SELECT greatest(60, :n / 500) AS np \gset

CREATE TABLE dims (id int PRIMARY KEY, tier text NOT NULL, active boolean NOT NULL, settings jsonb NOT NULL);
INSERT INTO dims SELECT p, CASE WHEN p % 3 = 0 THEN 'tier_a' WHEN p % 3 = 1 THEN 'tier_b' ELSE 'tier_c' END, p % 7 <> 0,
	jsonb_build_object('tier', CASE WHEN p <= 2 THEN 'tier_a' WHEN p <= :np / 2 THEN 'tier_b' ELSE 'tier_c' END, 'slot', p % 3) FROM generate_series(1, :np) p;

CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, skew_id int NOT NULL, ts date NOT NULL, kind text NOT NULL, amount float8 NOT NULL, qty int NOT NULL,
	deleted_at timestamptz, doc jsonb NOT NULL, memo text NOT NULL) WITH (autovacuum_enabled = false);
-- parent_id is flat; skew_id is log-uniform (parent 1 owns about one row in six); amount is a multiple of 0.25 (sums are exact), the metric m_a one decimal (sums round)
INSERT INTO events SELECT i, 1 + (i * 7919) % :np, floor(power(:np + 1.0, (i * 0.6180339887498949) % 1.0))::int,
	date '2024-01-01' + (hashint4(i + :seed) & 1023) % 730, 'cat_' || chr(97 + i % 4), 1 + (i % 17) + 0.25 * (i % 4), 1 + i % 9, CASE WHEN i % 37 = 0 THEN now() END,
	jsonb_build_object(
		'parts', jsonb_build_array(
			jsonb_build_object('kind', 'cat_' || chr(97 + i % 4), 'sub', 'catg_' || chr(97 + i % 6), 'dir_a', CASE WHEN i % 3 = 0 THEN 'neg' ELSE 'pos' END, 'score', 1 + (i * 3) % 10),
			jsonb_build_object('kind', 'cat_' || chr(97 + (i + 1) % 4), 'sub', 'catg_' || chr(97 + (i + 2) % 6), 'dir_a', CASE WHEN i % 5 = 0 THEN 'neg' ELSE 'pos' END, 'score', 1 + (i * 7) % 10)),
		'metrics', jsonb_build_object('m_a', ((i * 13) % 100) / 10.0, 'catg_a', 1 + (i * 3) % 10, 'catg_b', 1 + (i * 5) % 10)),
	(ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + i % 8] || ' ' || (ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + (i * 3 + 1) % 8]
		|| ' ' || (ARRAY['relay', 'cord', 'clamp', 'bolt', 'valve'])[1 + (i * 7) % 5]
	FROM generate_series(1, :n) i;
CREATE INDEX events_parent ON events (parent_id, ts, deleted_at);
CREATE INDEX events_skew ON events (skew_id, ts, deleted_at);
CREATE INDEX events_col ON events USING sift_col (id, parent_id, skew_id, ts, kind, amount, qty, deleted_at, doc)
	WITH (paths = 'doc.metrics.* float8, doc.parts[]{kind astext, sub astext, dir_a astext, score float8}');
CREATE INDEX events_memo ON events USING sift (memo);

CREATE TABLE facts (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, role text NOT NULL, val float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + (i * 104729) % :np, date '2024-01-01' + (hashint4(i + :seed + 7) & 1023) % 730, 'role_' || chr(97 + i % 3), 1 + (i % 11) + 0.5 * (i % 2)
	FROM generate_series(1, :n / 3) i;
CREATE INDEX facts_parent ON facts (parent_id, ts);
CREATE INDEX facts_col ON facts USING sift_col (id, parent_id, ts, role, val);

CREATE TABLE buckets (bucket date PRIMARY KEY, span text NOT NULL);
INSERT INTO buckets SELECT d::date, CASE WHEN d < date '2025-01-01' THEN 'span_b' ELSE 'span_a' END FROM generate_series(date '2024-01-01', date '2025-12-01', interval '1 month') d;

-- the events again, in four partitions by the half year of ts
CREATE TABLE events_part (id int NOT NULL, parent_id int NOT NULL, ts date NOT NULL, kind text NOT NULL, amount float8 NOT NULL, deleted_at timestamptz) PARTITION BY RANGE (ts);
CREATE TABLE events_part_1 PARTITION OF events_part FOR VALUES FROM ('2024-01-01') TO ('2024-07-01') WITH (autovacuum_enabled = false);
CREATE TABLE events_part_2 PARTITION OF events_part FOR VALUES FROM ('2024-07-01') TO ('2025-01-01') WITH (autovacuum_enabled = false);
CREATE TABLE events_part_3 PARTITION OF events_part FOR VALUES FROM ('2025-01-01') TO ('2025-07-01') WITH (autovacuum_enabled = false);
CREATE TABLE events_part_4 PARTITION OF events_part FOR VALUES FROM ('2025-07-01') TO ('2026-01-01') WITH (autovacuum_enabled = false);
INSERT INTO events_part SELECT id, parent_id, ts, kind, amount, deleted_at FROM events;
SELECT format('CREATE INDEX %1$s_parent ON %1$s (parent_id, ts, deleted_at); CREATE INDEX %1$s_col ON %1$s USING sift_col (id, parent_id, ts, kind, amount, deleted_at)', c.relname) AS ddl
	FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid ORDER BY c.relname \gexec

VACUUM (FREEZE, ANALYZE) dims, events, facts, buckets, events_part;
