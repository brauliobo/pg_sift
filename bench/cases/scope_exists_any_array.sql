-- a membership test per row: EXISTS over the links of a set where the link names the row or a group that contains it, the groups of the row read by `= ANY(ARRAY(sub-select))`
-- label: `WHERE EXISTS (SELECT 1 FROM links WHERE set_id = 4 AND (parent_id = d.id OR group_id = ANY(ARRAY(SELECT group_id FROM group_links WHERE parent_id = d.id))))` over the first 400 dimension rows (the cost of the statement grows with their square), ORDER BY label LIMIT 25
-- ordered: yes
--@ setup
CREATE TABLE ea_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int, group_id int, deleted_at timestamptz);
CREATE TABLE ea_group_links (group_id int NOT NULL, parent_id int NOT NULL, deleted_at timestamptz);
INSERT INTO ea_links SELECT p, 4, p, NULL, NULL FROM generate_series(1, (SELECT count(*) * 40 / 100 FROM dims)) p;
INSERT INTO ea_links SELECT 100000 + g, 4, NULL, 760 + g, NULL FROM generate_series(0, 9) g;
INSERT INTO ea_group_links SELECT 760 + p % 10, p, CASE WHEN p % 13 = 0 THEN now() END FROM generate_series(1, (SELECT count(*) FROM dims)) p;
CREATE INDEX ea_links_set ON ea_links (set_id, parent_id) WHERE deleted_at IS NULL;
CREATE INDEX ea_links_group ON ea_links (group_id, set_id) WHERE deleted_at IS NULL;
ANALYZE ea_links, ea_group_links
--@ pg
SELECT d.id, d.tier, 'dim ' || d.id AS label FROM dims d
WHERE d.id <= 400 AND EXISTS (SELECT 1 FROM ea_links l WHERE l.deleted_at IS NULL AND l.set_id = 4 AND (l.parent_id = d.id OR l.group_id = ANY(ARRAY(SELECT g.group_id FROM ea_group_links g WHERE g.deleted_at IS NULL AND g.parent_id = d.id))))
ORDER BY label, d.id LIMIT 25
