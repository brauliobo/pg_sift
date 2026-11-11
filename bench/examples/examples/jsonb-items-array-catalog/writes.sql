-- batch: append-docs
-- pattern: append_only
-- rows: nb
INSERT INTO doc SELECT * FROM gen_doc(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: catalog-recategorize
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE catalog SET category = 'w' || (1 + (sku + :round) % :card), price = price + 1 WHERE sku % 10 = :round % 10;

-- batch: doc-add-item
-- pattern: key_churn
-- rows: nf / 50
UPDATE doc SET items = items || jsonb_build_array(jsonb_build_object('sku', 1 + (id + :round) % :nd, 'qty', 1)) WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: doc-replace-items
-- pattern: key_churn
-- rows: nf / 50
UPDATE doc SET items = jsonb_build_array(jsonb_build_object('sku', 1 + (id * 3 + :round) % :nd, 'qty', 2)) WHERE id <= :nf AND id % 50 = (:round + 5) % 50;

-- batch: delete-docs
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM doc WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-docs
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO doc SELECT * FROM gen_doc(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) doc;
REINDEX TABLE CONCURRENTLY doc;
