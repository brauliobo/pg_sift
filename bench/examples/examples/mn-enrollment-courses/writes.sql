-- batch: enroll
-- pattern: append_only
-- rows: nb
INSERT INTO enrollment SELECT * FROM gen_enrollment(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: course-reclassify
-- pattern: dim_attr_churn
-- rows: nd / 100
UPDATE course SET dept = 'w' || (1 + (id + :round) % 8), level = 1 + (level + :round) % 4 WHERE id % 3 = :round % 3;

-- batch: student-cohort-change
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE student SET cohort = 'w' || (1 + (id + :round) % :card), country = 'c' || (1 + (id + :round) % 12) WHERE id % 10 = :round % 10;

-- batch: transfer-enrollments
-- pattern: key_churn
-- rows: nf / 50
UPDATE enrollment SET course_id = 1 + (course_id + :round) % (SELECT count(*) FROM course) WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: drop-enrollments
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM enrollment WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: re-enroll
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO enrollment SELECT * FROM gen_enrollment(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) enrollment;
REINDEX TABLE CONCURRENTLY enrollment;
