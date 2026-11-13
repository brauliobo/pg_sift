-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: merge-dimensions
-- pattern: key_churn
-- concurrent: no
-- rows: nf / 2
UPDATE fact SET dim_id = (dim_id + 1) / 2 WHERE dim_id > 1 AND dim_id % 2 = 0;
DELETE FROM dim WHERE id % 2 = 0 AND id > 1;

-- batch: split-dimensions
-- pattern: key_churn
-- concurrent: no
-- rows: nd / 2
INSERT INTO dim SELECT * FROM gen_dim(1, :nd) g WHERE g.id % 2 = 0 AND g.id > 1;
UPDATE fact SET dim_id = dim_id * 2 WHERE id % 3 = 0 AND dim_id * 2 <= :nd AND dim_id > 1;

-- batch: fact-change-dim
-- pattern: key_churn
-- rows: nf / 20
UPDATE fact SET dim_id = 1 + (dim_id + :round) % :nd WHERE id <= :nf AND id % 20 = :round % 20;

-- batch: dim-attribute-churn
-- pattern: dim_attr_churn
-- rows: nd / 3 + 1
UPDATE dim SET state = 'w' || (1 + (id + :round) % 5), flag = NOT flag WHERE id % 3 = :round % 3;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) fact;
REINDEX TABLE CONCURRENTLY fact;
