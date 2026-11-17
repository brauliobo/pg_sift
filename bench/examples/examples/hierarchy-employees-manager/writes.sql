-- batch: append-timesheets
-- pattern: append_only
-- rows: nb
INSERT INTO timesheet SELECT * FROM gen_timesheet(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: unit-restructure
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE employee SET dept = 'w' || (1 + (id + :round) % :card), level = 1 + (level + :round) % 6 WHERE id % 10 = :round % 10;

-- batch: reassign-managers
-- pattern: key_churn
-- rows: nd / 20
UPDATE employee SET manager_id = 1 + (id * 7 + :round) % (id - 1) WHERE id > 1 AND id % 20 = :round % 20;

-- batch: deactivate-employees
-- pattern: soft_delete
-- rows: nd / 10
UPDATE employee SET active = NOT active WHERE id % 10 = :round % 10;

-- batch: delete-timesheets
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM timesheet WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-timesheets
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO timesheet SELECT * FROM gen_timesheet(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO timesheet SELECT * FROM gen_timesheet(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) employee;
REINDEX TABLE CONCURRENTLY employee;
