-- batch: append-children
-- pattern: append_only
-- rows: nb
INSERT INTO child SELECT * FROM gen_child(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: parent-attribute-update
-- pattern: dim_attr_churn
-- rows: nd / 5 + 1
UPDATE parent SET state = 'w' || (1 + (id + :round) % 5), rank = (rank + 7) % 100 WHERE id % 5 = :round % 5;

-- batch: parent-key-change
-- pattern: key_churn
-- rows: nd / 97 + 1
UPDATE parent SET id = CASE WHEN id < 1000000 THEN id + 1000000 ELSE id - 1000000 END WHERE id % 1000000 % 97 = :round % 97;

-- batch: delete-children
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM child WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: restore-children
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO child SELECT * FROM gen_child(1, :nf) WHERE id % 10 = (:round - 1) % 10;

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

-- batch: vacuum-reindex-parent
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) parent;
VACUUM (ANALYZE) child;
REINDEX TABLE CONCURRENTLY parent;
