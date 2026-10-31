-- the newest snapshot of every series with its change since the previous and the first one: two aggregate arms (own and partner series) over a snapshot table, three window functions over the arms, the newest row of each series kept
-- label: `UNION ALL` of two `GROUP BY parent, variant, source, to_char(day, 'YYYY-MM'), created_at::date` arms (avg/min/max/count, about 5 rows a group) over a 100,000-row snapshot table of 100 parents, `lag`/`first_value`/`count`/`row_number` windows, `recency = 1`, a membership sub-select per arm
-- ordered: yes
--@ setup
CREATE TABLE ss_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL);
INSERT INTO ss_links SELECT p, 4, p FROM generate_series(1, 85) p;
CREATE INDEX ss_links_set ON ss_links (set_id, parent_id);
CREATE TABLE ss_snaps AS SELECT i AS id, 1 + (hashint4(i) & 16777215) % 100 AS parent_id, 1 + (hashint4(i + 1) & 16777215) % 2 AS variant_id, 1 + (hashint4(i + 2) & 16777215) % 2 AS lookup_id,
  DATE '2026-11-01' + (hashint4(i + 3) & 16777215) % 60 AS stay_day, 60 + ((hashint4(i + 4) & 16777215) % 4000) / 100.0 AS price,
  timestamp '2026-10-01' + (hashint4(i + 5) & 16777215) % 27 * interval '1 day' + (hashint4(i + 6) & 16777215) % 36000 * interval '1 second' AS created_at
  FROM generate_series(1, 100000) i;
ALTER TABLE ss_snaps ADD COLUMN partner boolean;
UPDATE ss_snaps SET partner = parent_id % 5 = 0;
ALTER TABLE ss_snaps ALTER COLUMN partner SET NOT NULL;
CREATE INDEX ss_snaps_series ON ss_snaps (parent_id, variant_id, lookup_id, stay_day, created_at);
CREATE INDEX ss_snaps_col ON ss_snaps USING sift_col (parent_id, variant_id, lookup_id, stay_day, price, created_at, partner);
VACUUM (FREEZE, ANALYZE) ss_snaps;
ANALYZE ss_links
--@ pg
WITH snap AS (
  (SELECT parent_id, variant_id, lookup_id, partner, to_char(stay_day, 'YYYY-MM') AS month, created_at::date AS taken_on, avg(price)::float AS avg_price, min(price)::float AS min_price, max(price)::float AS max_price, count(*) AS n
   FROM ss_snaps WHERE stay_day BETWEEN DATE '2026-11-01' AND DATE '2026-12-29' AND price IS NOT NULL AND parent_id IN (SELECT parent_id FROM ss_links WHERE set_id = 4) AND partner IS FALSE
   GROUP BY parent_id, variant_id, lookup_id, partner, to_char(stay_day, 'YYYY-MM'), created_at::date)
  UNION ALL
  (SELECT parent_id, variant_id, lookup_id, partner, to_char(stay_day, 'YYYY-MM'), created_at::date, avg(price)::float, min(price)::float, max(price)::float, count(*)
   FROM ss_snaps WHERE stay_day BETWEEN DATE '2026-11-01' AND DATE '2026-12-29' AND price IS NOT NULL AND parent_id IN (SELECT parent_id FROM ss_links WHERE set_id = 4) AND partner IS TRUE
   GROUP BY parent_id, variant_id, lookup_id, partner, to_char(stay_day, 'YYYY-MM'), created_at::date)),
win AS (
  SELECT snap.*, lag(avg_price) OVER w AS prev_avg, first_value(avg_price) OVER w AS first_avg,
    count(*) OVER (PARTITION BY partner, parent_id, variant_id, lookup_id, month) AS snapshots,
    row_number() OVER (PARTITION BY partner, parent_id, variant_id, lookup_id, month ORDER BY taken_on DESC) AS recency
  FROM snap WINDOW w AS (PARTITION BY partner, parent_id, variant_id, lookup_id, month ORDER BY taken_on))
SELECT partner, parent_id, variant_id, lookup_id, month, round(avg_price::numeric, 2) AS avg_price, round(min_price::numeric, 2) AS min_price, round(max_price::numeric, 2) AS max_price, snapshots,
  round(prev_avg::numeric, 2) AS prev_avg, round(((avg_price - prev_avg) / nullif(prev_avg, 0) * 100)::numeric, 2) AS pct_vs_prev, round(((avg_price - first_avg) / nullif(first_avg, 0) * 100)::numeric, 2) AS pct_from_first
FROM win WHERE recency = 1 ORDER BY partner, parent_id, variant_id, lookup_id, month
