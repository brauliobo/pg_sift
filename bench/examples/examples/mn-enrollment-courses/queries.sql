-- query: grade-by-dept-and-cohort
-- kind: group_by_dim_attr
-- join: inner
SELECT c.dept, s.cohort, count(*) AS n, sum(e.grade) AS grade FROM enrollment e JOIN student s ON s.id = e.student_id JOIN course c ON c.id = e.course_id GROUP BY c.dept, s.cohort;

-- query: distinct-students-by-dept
-- kind: count_distinct_across_join
-- join: inner
SELECT c.dept, count(DISTINCT e.student_id) AS students, count(DISTINCT e.course_id) AS courses FROM enrollment e JOIN course c ON c.id = e.course_id GROUP BY c.dept;

-- query: students-in-two-depts
-- kind: set_algebra
-- join: inner
SELECT count(*) AS students FROM (SELECT e.student_id FROM enrollment e JOIN course c ON c.id = e.course_id WHERE c.dept IN ('w1', 'w2') GROUP BY e.student_id HAVING count(DISTINCT c.dept) = 2) x;

-- query: students-never-enrolled
-- kind: anti_join_count
-- join: anti
SELECT s.country, count(*) AS students FROM student s WHERE NOT EXISTS (SELECT 1 FROM enrollment e WHERE e.student_id = s.id) GROUP BY s.country;

-- query: advanced-level-students-semi
-- kind: exists_scope
-- join: semi
SELECT s.cohort, count(*) AS students FROM student s WHERE EXISTS (SELECT 1 FROM enrollment e JOIN course c ON c.id = e.course_id WHERE e.student_id = s.id AND c.level = 4 AND e.grade >= 90) GROUP BY s.cohort;

-- query: top-courses-by-grade
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT c.id, c.dept, round(avg(e.grade), 4) AS avg_grade, count(*) AS n FROM enrollment e JOIN course c ON c.id = e.course_id GROUP BY c.id, c.dept HAVING count(*) >= 5 ORDER BY avg_grade DESC, c.id LIMIT 10;

-- query: co-enrollment-pairs
-- kind: self_join
-- join: inner
SELECT count(*) AS pairs FROM enrollment a JOIN enrollment b ON b.student_id = a.student_id AND b.course_id > a.course_id AND b.term = a.term WHERE a.course_id <= 3 AND b.course_id <= 6;

-- query: honors-by-level-and-country
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT c.level, count(*) FILTER (WHERE e.grade >= 90) AS honors, count(*) FILTER (WHERE s.country = 'c1') AS country1, count(*) AS n FROM enrollment e JOIN student s ON s.id = e.student_id JOIN course c ON c.id = e.course_id GROUP BY c.level;

-- query: courses-by-dept-right
-- kind: group_by_dim_attr
-- join: right
SELECT c.dept, count(e.id) AS enrollments, count(*) AS rows FROM enrollment e RIGHT JOIN course c ON c.id = e.course_id GROUP BY c.dept;
