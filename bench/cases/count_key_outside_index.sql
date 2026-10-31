-- `count(id)` of a NOT NULL primary key that the column index does not hold, in a grouped series: the same answer as `count(*)`, a normal plan today
-- label: `GROUP BY parent_id, variant_id, to_char(day, 'YYYY-MM'), created_at::date` with `avg(price), count(id)` over a 100,000-row snapshot table whose primary key is not a column of the sift_col index, a membership sub-select
-- ordered: yes
--@ setup
CREATE TABLE ck_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL);
INSERT INTO ck_links SELECT p, 4, p FROM generate_series(1, (SELECT count(*) * 85 / 100 FROM dims)) p;
CREATE INDEX ck_links_set ON ck_links (set_id, parent_id);
CREATE TABLE ck_snaps AS SELECT i AS id, 1 + (hashint4(i) & 16777215) % (SELECT count(*) FROM dims)::int AS parent_id, 1 + (hashint4(i + 1) & 16777215) % 4 AS variant_id,
  DATE '2026-11-01' + (hashint4(i + 3) & 16777215) % 60 AS day, 60 + ((hashint4(i + 4) & 16777215) % 4000) / 100.0 AS price,
  timestamp '2026-10-01' + (hashint4(i + 5) & 16777215) % 27 * interval '1 day' + (hashint4(i + 6) & 16777215) % 36000 * interval '1 second' AS created_at
  FROM generate_series(1, 100000) i;
ALTER TABLE ck_snaps ADD PRIMARY KEY (id);
CREATE INDEX ck_snaps_col ON ck_snaps USING sift_col (parent_id, variant_id, day, price, created_at);
VACUUM (FREEZE, ANALYZE) ck_snaps;
ANALYZE ck_links
--@ pg
SELECT parent_id, variant_id, to_char(day, 'YYYY-MM') AS month, created_at::date AS taken_on, round(avg(price)::numeric, 2) AS avg_price, count(id) AS n FROM ck_snaps
WHERE day BETWEEN DATE '2026-11-01' AND DATE '2026-12-29' AND price IS NOT NULL AND parent_id IN (SELECT parent_id FROM ck_links WHERE set_id = 4)
GROUP BY parent_id, variant_id, to_char(day, 'YYYY-MM'), created_at::date ORDER BY parent_id, variant_id, month, taken_on
