-- a self-referencing hierarchy: employee.manager_id -> employee.id (a tree of fan-out 5), timesheets as the fact
CREATE TABLE employee  (id int PRIMARY KEY, manager_id int, dept text NOT NULL, level int NOT NULL, active boolean NOT NULL);
CREATE TABLE timesheet (id bigint PRIMARY KEY, employee_id int NOT NULL, worked_on date NOT NULL, hours int NOT NULL);
