-- batch: append-listings
-- pattern: append_only
-- rows: nb
INSERT INTO listing SELECT * FROM gen_listing(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: leaf-reparent
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE category SET parent_id = :nd + 1 + (parent_id + :round) % (SELECT count(*) FROM category WHERE depth = 1) WHERE depth = 2 AND id % 10 = :round % 10;

-- batch: branch-reparent
-- pattern: dim_attr_churn
-- rows: nd / 100
UPDATE category SET parent_id = (SELECT min(id) FROM category WHERE depth = 0) + (parent_id + :round) % (SELECT count(*) FROM category WHERE depth = 0) WHERE depth = 1 AND id % 5 = :round % 5;

-- batch: root-rename
-- pattern: dim_attr_churn
-- rows: 1
UPDATE category SET name = 'root' || (1 + (id + :round) % 3) WHERE depth = 0;

-- batch: listing-move-leaf
-- pattern: key_churn
-- rows: nf / 50
UPDATE listing SET category_id = 1 + (category_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: delete-listings
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM listing WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-listings
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO listing SELECT * FROM gen_listing(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) listing;
REINDEX TABLE CONCURRENTLY listing;
