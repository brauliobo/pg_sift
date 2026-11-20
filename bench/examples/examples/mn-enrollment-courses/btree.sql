CREATE INDEX enrollment_student_idx ON enrollment (student_id, course_id);
CREATE INDEX enrollment_course_idx ON enrollment (course_id, student_id);
CREATE INDEX course_dept_idx ON course (dept);
