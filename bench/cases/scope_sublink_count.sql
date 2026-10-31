-- the scope of a request as a sub-select that resolves direct and group memberships with a LEFT JOIN and a coalesce: the aggregate over the filtered table
-- label: `count(*), sum(amount) .. parent_id IN (SELECT coalesce(l.parent_id, g.parent_id) FROM links l LEFT JOIN group_links g ON .. WHERE l.set_id = 4)` over events joined to dims, 85% of the parents in the set
-- ordered: yes
--@ setup
CREATE TABLE sl_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int, group_id int, deleted_at timestamptz);
CREATE TABLE sl_group_links (group_id int NOT NULL, parent_id int NOT NULL, deleted_at timestamptz);
INSERT INTO sl_links SELECT p, 4, p, NULL, NULL FROM generate_series(1, (SELECT count(*) * 85 / 100 FROM dims)) p;
INSERT INTO sl_links SELECT 100000 + g, 3, NULL, 760 + g, NULL FROM generate_series(0, 1) g;
INSERT INTO sl_group_links SELECT 760 + g, p, CASE WHEN p % 11 = 0 THEN now() END FROM generate_series(0, 1) g, generate_series(1, 150) p;
CREATE INDEX sl_links_set ON sl_links (set_id, parent_id);
ANALYZE sl_links, sl_group_links
--@ pg
SELECT d.tier, count(*) AS n, sum(e.amount) AS total FROM events e JOIN dims d ON d.id = e.parent_id AND d.active
WHERE e.deleted_at IS NULL AND e.ts >= date '2025-01-01'
  AND e.parent_id IN (SELECT coalesce(l.parent_id, g.parent_id) FROM sl_links l LEFT JOIN sl_group_links g ON g.group_id = l.group_id AND g.deleted_at IS NULL WHERE l.set_id = 4 AND l.deleted_at IS NULL)
GROUP BY d.tier ORDER BY d.tier
