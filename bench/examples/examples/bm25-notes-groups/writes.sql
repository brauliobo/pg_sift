-- batch: append-notes
-- pattern: append_only
-- rows: nb
INSERT INTO note SELECT * FROM gen_note(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: group-reclassify
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE grp SET kind = 'w' || (1 + (id + :round) % :card), tier = 1 + (tier + :round) % 4 WHERE id % 10 = :round % 10;

-- batch: note-edit-title
-- pattern: dim_attr_churn
-- rows: nf / 20
UPDATE note SET title = replace(title, 'w1 ', 'w7 ') WHERE id <= :nf AND id % 20 = :round % 20;

-- batch: note-move-group
-- pattern: key_churn
-- rows: nf / 50
UPDATE note SET grp_id = 1 + (grp_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: delete-notes
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM note WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-notes
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO note SELECT * FROM gen_note(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) note;
REINDEX TABLE CONCURRENTLY note;
