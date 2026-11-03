-- polling by a cursor: the rows of a set of parents changed after a timestamp, newest change first, a page of 500
-- label: `WHERE updated_at >= cursor AND parent_id IN (set) AND deleted_at IS NULL ORDER BY updated_at DESC, id LIMIT 500` with a lateral sub-select on the dimension row, a btree on `(updated_at, id)`
-- ordered: yes
--@ setup
CREATE TABLE pc_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL);
INSERT INTO pc_links SELECT p, 4, p FROM generate_series(1, (SELECT count(*) * 85 / 100 FROM dims)) p;
CREATE INDEX pc_links_set ON pc_links (set_id, parent_id);
CREATE TABLE pc_events AS SELECT id, parent_id, ts, amount, deleted_at, ts + (hashint4(id) & 1048575) % 90 * interval '1 day' + (hashint4(id + 1) & 86399) * interval '1 second' AS updated_at FROM events;
ALTER TABLE pc_events ADD PRIMARY KEY (id);
CREATE INDEX pc_events_upd ON pc_events (updated_at, id);
CREATE INDEX pc_events_col ON pc_events USING sift_col (id, parent_id, ts, amount, deleted_at, updated_at);
VACUUM (FREEZE, ANALYZE) pc_events;
ANALYZE pc_links
--@ pg
SELECT e.id, e.updated_at, e.amount, d.tier, d.settings
FROM pc_events e JOIN dims d ON d.id = e.parent_id
LEFT JOIN LATERAL (SELECT dd.tier AS own_tier FROM dims dd WHERE dd.id = e.parent_id) own ON true
WHERE e.deleted_at IS NULL AND e.updated_at >= timestamptz '2025-12-15' AND e.ts >= date '2025-01-01' AND e.parent_id IN (SELECT parent_id FROM pc_links WHERE set_id = 4)
ORDER BY e.updated_at DESC NULLS LAST, e.id LIMIT 500
