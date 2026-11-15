-- batch: append-documents
-- pattern: append_only
-- rows: nb
INSERT INTO document SELECT * FROM gen_document(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: author-reclassify
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE author SET country = 'c' || (1 + (id + :round) % 12), tier = 1 + (tier + :round) % 4 WHERE id % 10 = :round % 10;

-- batch: document-edit
-- pattern: dim_attr_churn
-- rows: nf / 20
UPDATE document SET body = replace(body, 'w1 ', 'w2 ') WHERE id <= :nf AND id % 20 = :round % 20;

-- batch: document-change-author
-- pattern: key_churn
-- rows: nf / 50
UPDATE document SET author_id = 1 + (author_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: delete-documents
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM document WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-documents
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO document SELECT * FROM gen_document(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO document SELECT * FROM gen_document(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) document;
REINDEX TABLE CONCURRENTLY document;
