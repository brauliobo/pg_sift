-- a page of the newest rows of a set of parents: top-3 by date over a join, with a lateral sub-select that aggregates the rows' own jsonb array and one that aggregates a child table
-- label: `.. LEFT JOIN LATERAL (aggregate over jsonb_to_recordset(doc['parts'])) .. LEFT JOIN LATERAL (avg over facts of the parent) .. WHERE parent_id IN (set of 340 parents) AND ts >= last 30 days ORDER BY ts DESC, id LIMIT 3`
-- ordered: yes
--@ setup
CREATE TABLE pn_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL);
INSERT INTO pn_links SELECT p, 4, p FROM generate_series(1, (SELECT count(*) * 85 / 100 FROM dims)) p;
CREATE INDEX pn_links_set ON pn_links (set_id, parent_id);
ANALYZE pn_links
--@ pg
SELECT e.id, e.ts, e.amount, d.tier, o.n_parts, o.avg_score, f.avg_val
FROM events e JOIN dims d ON d.id = e.parent_id AND d.active
LEFT JOIN LATERAL (SELECT count(*) AS n_parts, avg(x.score) AS avg_score FROM jsonb_to_recordset(e.doc['parts']) x(kind text, sub text, score float8) WHERE x.score IS NOT NULL) o ON true
LEFT JOIN LATERAL (SELECT avg(f.val) AS avg_val FROM facts f WHERE f.parent_id = e.parent_id AND f.ts >= date '2025-06-01') f ON true
WHERE e.deleted_at IS NULL AND e.parent_id IN (SELECT parent_id FROM pn_links WHERE set_id = 4) AND e.ts >= date '2025-12-01'
ORDER BY e.ts DESC NULLS LAST, e.id LIMIT 3
