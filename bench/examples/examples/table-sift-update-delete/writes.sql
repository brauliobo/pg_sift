-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: dim-attribute-churn
-- pattern: dim_attr_churn
-- rows: nd / 3 + 1
UPDATE dim SET state = 'w' || (1 + (id + :round) % 5), flag = NOT flag WHERE id % 3 = :round % 3;

-- batch: fact-change-dim
-- pattern: key_churn
-- rows: nf / 50
UPDATE fact SET dim_id = 1 + (dim_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: delete-facts
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM fact WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-facts
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO fact SELECT * FROM gen_fact(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rounds: 3
-- rows: 0
VACUUM (ANALYZE) fact;


-- batch: hot-dims-flip-x20
-- pattern: dim_attr_churn
-- rows: 100
UPDATE dim SET state = 'w' || (1 + (id + :round + 0) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 1) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 2) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 3) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 4) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 5) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 6) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 7) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 8) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 9) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 10) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 11) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 12) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 13) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 14) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 15) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 16) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 17) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 18) % 5), flag = NOT flag WHERE id <= 5;
UPDATE dim SET state = 'w' || (1 + (id + :round + 19) % 5), flag = NOT flag WHERE id <= 5;

-- batch: all-dims-flip-x5
-- pattern: dim_attr_churn
-- rows: 250
UPDATE dim SET state = 'w' || (1 + (id + :round + 0) % 5), flag = NOT flag WHERE id <= 50;
UPDATE dim SET state = 'w' || (1 + (id + :round + 1) % 5), flag = NOT flag WHERE id <= 50;
UPDATE dim SET state = 'w' || (1 + (id + :round + 2) % 5), flag = NOT flag WHERE id <= 50;
UPDATE dim SET state = 'w' || (1 + (id + :round + 3) % 5), flag = NOT flag WHERE id <= 50;
UPDATE dim SET state = 'w' || (1 + (id + :round + 4) % 5), flag = NOT flag WHERE id <= 50;

-- batch: flip-between-appends
-- pattern: dim_attr_churn
-- rows: 5 * nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 0 * :nb / 5 + 1, :nf + :round * :nf + (0 + 1) * :nb / 5);
UPDATE dim SET state = 'w' || (1 + (id + :round + 0) % 5), flag = NOT flag WHERE id <= 5;
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1 * :nb / 5 + 1, :nf + :round * :nf + (1 + 1) * :nb / 5);
UPDATE dim SET state = 'w' || (1 + (id + :round + 1) % 5), flag = NOT flag WHERE id <= 5;
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 2 * :nb / 5 + 1, :nf + :round * :nf + (2 + 1) * :nb / 5);
UPDATE dim SET state = 'w' || (1 + (id + :round + 2) % 5), flag = NOT flag WHERE id <= 5;
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 3 * :nb / 5 + 1, :nf + :round * :nf + (3 + 1) * :nb / 5);
UPDATE dim SET state = 'w' || (1 + (id + :round + 3) % 5), flag = NOT flag WHERE id <= 5;
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 4 * :nb / 5 + 1, :nf + :round * :nf + (4 + 1) * :nb / 5);
UPDATE dim SET state = 'w' || (1 + (id + :round + 4) % 5), flag = NOT flag WHERE id <= 5;

-- batch: update-amounts
-- pattern: dim_attr_churn
-- rows: nf / 20
UPDATE fact SET amount = amount + 1, status = (ARRAY['new', 'paid', 'done', 'void'])[1 + (id + :round) % 4] WHERE id <= :nf AND id % 20 = :round % 20;

-- batch: update-and-delete-mix
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 5
UPDATE fact SET dim_id = 1 + (dim_id + :round) % :nd WHERE id <= :nf AND id % 10 = (:round + 3) % 10;
DELETE FROM fact WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: drop-recreate-sift-index
-- pattern: maintenance
-- maintenance: yes
-- rounds: 3
-- sift_only: yes
-- rows: 0
DROP INDEX CONCURRENTLY dim_col;
CREATE INDEX CONCURRENTLY dim_col ON dim USING sift_col (id, grp, state, flag, weight);

-- batch: ddl-reloption
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
ALTER TABLE dim SET (autovacuum_vacuum_scale_factor = 0.15);
ALTER TABLE dim RESET (autovacuum_vacuum_scale_factor);
