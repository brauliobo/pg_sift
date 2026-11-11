-- batch: append-children
-- pattern: append_only
-- rows: nb
INSERT INTO child SELECT * FROM gen_child(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: parent-read-column-update
-- pattern: dim_attr_churn
-- rows: nd / 5 + 1
UPDATE parent SET opened = date '2020-01-01' + ((opened - date '2020-01-01') + :round) % 1000 WHERE id % 5 = :round % 5;

-- batch: parent-other-column-update
-- pattern: dim_attr_churn
-- rows: nd / 7 + 1
UPDATE parent SET name = 'n' || ((id + :round) % 3) WHERE id % 7 = :round % 7;

-- batch: parent-key-change
-- pattern: key_churn
-- rows: nd / 97 + 1
UPDATE parent SET id = CASE WHEN id < 1000000 THEN id + 1000000 ELSE id - 1000000 END WHERE id % 1000000 % 97 = :round % 97;

-- batch: parent-delete
-- pattern: delete_heavy
-- concurrent: no
-- rows: nd / 101 + 1
DELETE FROM parent WHERE id % 1000000 % 101 = :round % 101;

-- batch: parent-reinsert
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nd / 101 + 1
INSERT INTO parent SELECT * FROM gen_parent(1, :nd) g WHERE g.id % 101 = (:round - 1) % 101 AND NOT EXISTS (SELECT 1 FROM parent p WHERE p.id % 1000000 = g.id);

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO child SELECT * FROM gen_child(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: updates-between-appends
-- pattern: dim_attr_churn
-- rows: nb + 3 * nd / 4
INSERT INTO child SELECT * FROM gen_child(:nf + :round * :nf + 0 * :nb / 3 + 1, :nf + :round * :nf + 1 * :nb / 3);
UPDATE parent SET opened = date '2020-01-01' + ((opened - date '2020-01-01') + :round) % 1000 WHERE id % 4 = 0;
INSERT INTO child SELECT * FROM gen_child(:nf + :round * :nf + 1 * :nb / 3 + 1, :nf + :round * :nf + 2 * :nb / 3);
UPDATE parent SET opened = date '2020-01-01' + ((opened - date '2020-01-01') + :round) % 1000 WHERE id % 4 = 1;
INSERT INTO child SELECT * FROM gen_child(:nf + :round * :nf + 2 * :nb / 3 + 1, :nf + :round * :nf + 3 * :nb / 3);
UPDATE parent SET opened = date '2020-01-01' + ((opened - date '2020-01-01') + :round) % 1000 WHERE id % 4 = 2;

-- batch: refill-child
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) child;
REINDEX TABLE CONCURRENTLY child;
