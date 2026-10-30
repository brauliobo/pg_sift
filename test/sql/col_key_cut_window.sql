-- key_cut by default keeps the zones a window needs: events of 500 entities over 200 days, written day by day, so every entity has rows in every
-- unit and the date, which follows the order of the table, is what skips units. An equality on the entity reads every unit, but the cut by it would widen the zones of
-- the date, which no unique b-tree of the table answers: the build leaves the units by superblock, whether or not the table also has a unique id. A tenant scattered over
-- the units next to a primary key that follows the table is cut by, and the notice names the id whose zones widen.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE wev (id int PRIMARY KEY, dim_a_id int NOT NULL, day date NOT NULL, a1 int, a2 int, a3 int, a4 int, a5 int, a6 int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO wev SELECT i, 1 + (i - 1) % 500, date '2024-01-01' + (i - 1) / 500, (i * 31) % 1000, (i * 37) % 1000, (i * 41) % 1000, (i * 43) % 1000, (i * 47) % 1000, (i * 53) % 1000, repeat('x', 100)
FROM generate_series(1, 100000) i;
CREATE INDEX wev_entity ON wev (dim_a_id);
VACUUM (FREEZE, ANALYZE) wev;

CREATE FUNCTION kopt(i regclass) RETURNS text LANGUAGE sql AS $$ SELECT coalesce(reloptions::text, '') FROM pg_class WHERE oid = i $$;

-- with the id, and without it: the date keeps the units in both
CREATE INDEX wev_id ON wev USING sift_col (id, dim_a_id, day, a1, a2, a3, a4, a5, a6);
SELECT leaf, decision, reason FROM sift.key_cut_decision('wev_id');
CREATE INDEX wev_noid ON wev USING sift_col (dim_a_id, day, a1, a2, a3, a4, a5, a6);
SELECT leaf, decision, reason FROM sift.key_cut_decision('wev_noid');
SELECT kopt('wev_id') ~ 'key_cut=auto: ' AS id_cut, kopt('wev_noid') ~ 'key_cut=auto: ' AS noid_cut;

-- a primary key that follows the table does not hide a scattered tenant: the cut is chosen and the notice says which zones widen
CREATE TABLE wkp (id int PRIMARY KEY, tenant_id int, a1 int, a2 int, a3 int, a4 int, a5 int, a6 int, a7 int, a8 int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO wkp SELECT i, CASE WHEN random() < 0.15 THEN (random() * 100)::int ELSE i / 1250 END, (i * 31) % 1000, (i * 37) % 1000, (i * 41) % 1000, (i * 43) % 1000, (i * 47) % 1000, (i * 53) % 1000, (i * 59) % 1000, (i * 61) % 1000, repeat('x', 100)
FROM generate_series(1, 125000) i;
VACUUM (FREEZE, ANALYZE) wkp;
SET client_min_messages = notice;
CREATE INDEX wkp_id ON wkp USING sift_col (id, tenant_id, a1, a2, a3, a4, a5, a6, a7, a8);
SET client_min_messages = warning;
SELECT kopt('wkp_id') ~ 'key_cut=auto: tenant_id' AS cut_by_tenant;

DROP TABLE wev, wkp;
DROP FUNCTION kopt(regclass);
DROP EXTENSION pg_sift;
