CREATE INDEX employee_manager_idx ON employee (manager_id);
CREATE INDEX timesheet_employee_idx ON timesheet (employee_id, worked_on);
