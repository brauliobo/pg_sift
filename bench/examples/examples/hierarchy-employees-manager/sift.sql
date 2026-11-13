CREATE INDEX timesheet_col ON timesheet USING sift_col (employee_id, worked_on, hours);
CREATE INDEX employee_col ON employee USING sift_col (id, manager_id, dept, level, active);
