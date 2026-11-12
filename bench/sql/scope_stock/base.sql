-- The real-shaped fixture of bench/steps/scope_stock_arms.py (psql -v facts=2400000 -v members=2648 -f base.sql):
-- events (facts) of members (dim_a_id) with a skewed number of events, 32 tenants that reach members directly (grants) or through groups (grants + memberships),
-- a member in about 1.4 tenants, some soft-deleted rows. Physical order is by time, so a member is scattered over the whole heap. Deterministic (setseed).
\set ON_ERROR_STOP on
SET client_min_messages = warning;
SELECT setseed(0.5);
CREATE TABLE events (id int NOT NULL, dim_a_id int NOT NULL, created_at date NOT NULL, kind text NOT NULL, channel_id int NOT NULL,
                     region text NOT NULL, score int NOT NULL, amount int NOT NULL);
CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz);
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz);

-- members: log-normal event counts (median a few hundred, the largest about 1.5% of all events)
CREATE TEMP TABLE mem AS
  SELECT 1000 + g AS dim_a_id, exp(1.3 * sqrt(-2 * ln(1 - random())) * cos(2 * pi() * random())) AS w FROM generate_series(1, :member_total) g;
CREATE TEMP TABLE mem_n AS SELECT dim_a_id, greatest(1, round(w / sum(w) OVER () * :facts))::int AS n FROM mem;
CREATE UNIQUE INDEX ON mem_n (dim_a_id);

INSERT INTO events
SELECT row_number() OVER (ORDER BY created_at, r), dim_a_id, created_at, kind, channel_id, region, score, amount
  FROM (SELECT m.dim_a_id,
               date '2026-10-31' - floor(2860 * power(random(), 1.7))::int AS created_at,
               random() AS r,
               'k' || floor(random() * 6)::int AS kind,
               1 + floor(12 * power(random(), 1.5))::int AS channel_id,
               (ARRAY['US','GB','DE','FR','ES','IT','NL','BR','MX','CA','AU','JP','SE','NO','DK','PL','PT','CH','AT','BE'])[1 + floor(20 * power(random(), 2))::int] AS region,
               1 + floor(random() * 10)::int AS score,
               floor(random() * 500)::int AS amount
          FROM mem_n m, generate_series(1, m.n)) x;
ALTER TABLE events ADD PRIMARY KEY (id);

-- tenants: n members each, the first eight with the share of all events of a real access model (49, 20, 17, 6, 6, 4, 1, 0.1%); tenant 3 reaches its members through groups only
CREATE TEMP TABLE spec (tenant int PRIMARY KEY, n int NOT NULL, groups_only bool NOT NULL, share float8);
INSERT INTO spec VALUES (1, 1149, false, 0.494), (2, 569, false, 0.196), (3, 414, true, 0.174), (4, 268, false, 0.060),
                        (5, 17, false, 0.062), (6, 25, false, 0.039), (7, 44, false, 0.011), (8, 11, false, 0.001);
INSERT INTO spec SELECT t, 15 + floor(random() * 65)::int, t % 5 = 0, NULL FROM generate_series(9, 32) t;
CREATE TEMP TABLE pick (tenant int NOT NULL, dim_a_id int NOT NULL, PRIMARY KEY (tenant, dim_a_id));
DO $$
DECLARE r record; total bigint; got bigint; want float8; c int; k int;
BEGIN
  SELECT sum(n) INTO total FROM mem_n;
  FOR r IN SELECT * FROM spec ORDER BY tenant LOOP
    got := 0;
    FOR k IN 1..r.n LOOP
      want := CASE WHEN r.share IS NULL THEN NULL ELSE (r.share * total - got) / (r.n - k + 1) END;
      c := NULL;
      IF want IS NOT NULL THEN
        SELECT m.dim_a_id INTO c FROM mem_n m WHERE m.n BETWEEN want * 0.6 AND want * 1.4
           AND NOT EXISTS (SELECT 1 FROM pick p WHERE p.tenant = r.tenant AND p.dim_a_id = m.dim_a_id) ORDER BY random() LIMIT 1;
        IF c IS NULL THEN
          SELECT m.dim_a_id INTO c FROM mem_n m WHERE NOT EXISTS (SELECT 1 FROM pick p WHERE p.tenant = r.tenant AND p.dim_a_id = m.dim_a_id)
           ORDER BY abs(m.n - want) LIMIT 1;
        END IF;
      ELSE
        SELECT m.dim_a_id INTO c FROM mem_n m WHERE NOT EXISTS (SELECT 1 FROM pick p WHERE p.tenant = r.tenant AND p.dim_a_id = m.dim_a_id) ORDER BY random() LIMIT 1;
      END IF;
      INSERT INTO pick VALUES (r.tenant, c);
      got := got + (SELECT n FROM mem_n WHERE dim_a_id = c);
    END LOOP;
  END LOOP;
END $$;

-- 30% of the members of a tenant are granted directly, the rest through groups of about 45; 12% of the direct ones are also in a group of their tenant (two paths to one pair)
CREATE TEMP TABLE pick_r AS
  SELECT p.tenant, p.dim_a_id, row_number() OVER (PARTITION BY p.tenant ORDER BY random()) AS rn, count(*) OVER (PARTITION BY p.tenant) AS cnt, s.groups_only
    FROM pick p JOIN spec s ON s.tenant = p.tenant;
INSERT INTO grants (tenant_id, dim_a_id) SELECT tenant, dim_a_id FROM pick_r WHERE NOT groups_only AND rn <= 0.3 * cnt;
CREATE TEMP TABLE grp AS
  SELECT tenant, dim_a_id, tenant * 1000 + (row_number() OVER (PARTITION BY tenant ORDER BY random()) - 1) / 45 AS group_id
    FROM pick_r WHERE groups_only OR rn > 0.3 * cnt;
INSERT INTO grants (tenant_id, group_id) SELECT DISTINCT tenant, group_id FROM grp;
INSERT INTO memberships (group_id, dim_a_id) SELECT group_id, dim_a_id FROM grp;
INSERT INTO memberships (group_id, dim_a_id)
  SELECT g.tenant * 1000, p.dim_a_id FROM pick_r p JOIN (SELECT DISTINCT tenant FROM grp) g ON g.tenant = p.tenant
   WHERE NOT p.groups_only AND p.rn <= 0.3 * p.cnt AND random() < 0.12
  ON CONFLICT DO NOTHING;
-- soft-deleted rows that are not pairs now: revoked grants and memberships
INSERT INTO grants (tenant_id, dim_a_id, deleted_at)
  SELECT 1 + floor(random() * 32)::int, 1000 + 1 + floor(random() * :member_total)::int, now() FROM generate_series(1, 150);
INSERT INTO memberships (group_id, dim_a_id, deleted_at)
  SELECT (1 + floor(random() * 32)::int) * 1000, 1000 + 1 + floor(random() * :member_total)::int, now() FROM generate_series(1, 150);

CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE VIEW tenant_pairs AS
  SELECT g.dim_a_id AS member, g.tenant_id FROM grants g WHERE g.deleted_at IS NULL AND g.dim_a_id IS NOT NULL
  UNION
  SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
VACUUM (FREEZE, ANALYZE) events, grants, memberships;

SELECT (SELECT count(*) FROM events) AS events, (SELECT count(DISTINCT dim_a_id) FROM events) AS member_total,
       (SELECT count(*) FROM grants WHERE deleted_at IS NULL) AS grants, (SELECT count(*) FROM memberships WHERE deleted_at IS NULL) AS member_totalhips,
       (SELECT count(*) FROM tenant_pairs) AS pairs, (SELECT count(DISTINCT tenant_id) FROM tenant_pairs) AS tenants,
       round((SELECT count(*) FROM events e JOIN tenant_pairs p ON p.member = e.dim_a_id)::numeric / (SELECT count(*) FROM events), 2) AS entries_per_event;
SELECT p.tenant_id, count(*) AS member_total, round(100.0 * sum(c.n) / (SELECT sum(n) FROM mem_n), 2) AS pct_events
  FROM tenant_pairs p JOIN mem_n c ON c.dim_a_id = p.member GROUP BY 1 ORDER BY 3 DESC LIMIT 12;
