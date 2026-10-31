-- a text search ranked by score, filtered by a membership test of the matching row: the top 3 matches of a word pair within a set of parents
-- label: `WHERE memo @@> 'cable relay' AND EXISTS (SELECT 1 FROM links WHERE set_id = 4 AND parent_id = e.parent_id) ORDER BY sift.score(ctid) DESC LIMIT 3`
-- ordered: yes
--@ setup
CREATE TABLE sf_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL, deleted_at timestamptz);
INSERT INTO sf_links SELECT p, 4, p, NULL FROM generate_series(1, (SELECT count(*) * 30 / 100 FROM dims)) p;
CREATE INDEX sf_links_set ON sf_links (set_id, parent_id) WHERE deleted_at IS NULL;
ANALYZE sf_links
--@ pg
SELECT e.id, sift.score(e.ctid) AS score FROM events e
WHERE e.memo @@> 'cable relay' AND e.deleted_at IS NULL AND EXISTS (SELECT 1 FROM sf_links l WHERE l.deleted_at IS NULL AND l.set_id = 4 AND l.parent_id = e.parent_id)
ORDER BY sift.score(e.ctid) DESC, e.id LIMIT 3
