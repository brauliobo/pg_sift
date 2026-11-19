-- batch: append-facts
-- pattern: append_only
-- rows: nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: dim-attribute-churn
-- pattern: dim_attr_churn
-- rows: nd / 3 + 1
UPDATE dim SET state = 'w' || (1 + (id + :round) % 5), flag = NOT flag WHERE id % 3 = :round % 3;

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


-- batch: dim-flip-between-appends
-- pattern: dim_attr_churn
-- rows: 2 * nb
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);
UPDATE dim SET state = 'w' || (1 + (id + :round) % 5), flag = NOT flag WHERE id % 7 = :round % 7;
INSERT INTO fact SELECT * FROM gen_fact(:nf + :round * :nf + :nb + 1, :nf + :round * :nf + 2 * :nb);

-- batch: drop-recreate-sift-index
-- pattern: maintenance
-- maintenance: yes
-- rounds: 3
-- sift_only: yes
-- rows: 0
DROP INDEX CONCURRENTLY dim_col;
CREATE INDEX CONCURRENTLY dim_col ON dim USING sift_col (id, grp, state, flag, weight);
