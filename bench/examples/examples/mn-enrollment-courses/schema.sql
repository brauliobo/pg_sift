-- M:N with a link table that carries attributes: enrollment(student, course, grade, term) between student and course
CREATE TABLE student    (id int PRIMARY KEY, cohort text NOT NULL, country text NOT NULL);
CREATE TABLE course     (id int PRIMARY KEY, dept text NOT NULL, level int NOT NULL);
CREATE TABLE enrollment (id bigint PRIMARY KEY, student_id int NOT NULL, course_id int NOT NULL, grade int NOT NULL, term text NOT NULL);
