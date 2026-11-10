CREATE INDEX enrollment_col ON enrollment USING sift_col (student_id, course_id, grade, term);
CREATE INDEX student_col ON student USING sift_col (id, cohort, country);
CREATE INDEX course_col ON course USING sift_col (id, dept, level);
