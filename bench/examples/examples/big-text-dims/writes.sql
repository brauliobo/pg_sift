-- batch: append-views
-- pattern: append_only
-- rows: nb
INSERT INTO page_view SELECT * FROM gen_view(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: doc-rewrite
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE doc SET kind = 'w' || (1 + (id + :round) % :card), description = 'needle' || (1 + (id + :round) % 100) || ' ' || repeat(md5((id + :round)::text) || ' ', 30) WHERE id % 10 = :round % 10;

-- batch: doc-retitle
-- pattern: dim_attr_churn
-- rows: nd / 20
UPDATE doc SET title = 'Title ' || (1 + (id + :round) % 500) || ' ' || md5(id::text) WHERE id % 20 = :round % 20;

-- batch: page_view-change-doc
-- pattern: key_churn
-- rows: nf / 50
UPDATE page_view SET doc_id = 1 + (doc_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: delete-views
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM page_view WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-views
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO page_view SELECT * FROM gen_view(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) doc;
REINDEX TABLE CONCURRENTLY doc;
