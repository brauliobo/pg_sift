-- a date range over a daily series of a set of parents with a filter that is a flag or a non-null value, newest first, joined to the dimension rows
-- label: `.. WHERE parent_id IN (set) AND day BETWEEN last 8 days AND (available IS FALSE OR rank IS NOT NULL) ORDER BY day DESC` over 48,000 daily rows
-- ordered: yes
--@ setup
CREATE TABLE fr_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL);
INSERT INTO fr_links SELECT p, 4, p FROM generate_series(1, (SELECT count(*) * 85 / 100 FROM dims)) p;
CREATE INDEX fr_links_set ON fr_links (set_id, parent_id);
CREATE TABLE fr_ranks AS SELECT row_number() OVER () AS id, p AS parent_id, s AS lookup_id, date '2025-11-01' + d AS day,
  CASE WHEN hashint4((p * 1000 + d * 7 + s)::int) % 100 < 85 THEN 1 + abs(hashint4((p + d + s)::int)) % 300 END AS rank, hashint4((p + d * 3 + s)::int) % 10 <> 0 AS available
  FROM generate_series(1, (SELECT count(*) FROM dims)) p, generate_series(1, 2) s, generate_series(0, 59) d;
ALTER TABLE fr_ranks ADD PRIMARY KEY (id);
CREATE UNIQUE INDEX fr_ranks_uq ON fr_ranks (parent_id, lookup_id, day);
CREATE INDEX fr_ranks_col ON fr_ranks USING sift_col (parent_id, lookup_id, day, rank, available);
VACUUM (FREEZE, ANALYZE) fr_ranks;
ANALYZE fr_links
--@ pg
SELECT r.id, r.parent_id, r.lookup_id, r.day, r.rank, r.available, d.tier FROM fr_ranks r JOIN dims d ON d.id = r.parent_id
WHERE r.parent_id IN (SELECT parent_id FROM fr_links WHERE set_id = 4) AND r.day >= date '2025-12-23' AND r.day <= date '2025-12-30' AND (r.available IS FALSE OR r.rank IS NOT NULL)
ORDER BY r.day DESC, r.id
