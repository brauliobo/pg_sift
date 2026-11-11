-- batch: append-sales
-- pattern: append_only
-- rows: nb
INSERT INTO sale SELECT * FROM gen_sale(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: item-reparent
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE item SET category_id = 1 + (category_id + :round) % (SELECT count(*) FROM category) WHERE id % 10 = :round % 10;

-- batch: category-reparent
-- pattern: dim_attr_churn
-- rows: nd / 200
UPDATE category SET department_id = 1 + (department_id + :round) % (SELECT count(*) FROM department) WHERE id % 5 = :round % 5;

-- batch: department-reclassify
-- pattern: dim_attr_churn
-- rows: 1
UPDATE department SET division = 'w' || (1 + (id + :round) % 6) WHERE id % 2 = :round % 2;

-- batch: sale-move-item
-- pattern: key_churn
-- rows: nf / 50
UPDATE sale SET item_id = 1 + (item_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: delete-sales
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM sale WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-sales
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO sale SELECT * FROM gen_sale(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO sale SELECT * FROM gen_sale(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) sale;
REINDEX TABLE CONCURRENTLY sale;
