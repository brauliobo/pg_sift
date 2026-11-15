CREATE INDEX fact_col ON fact USING sift_col (status_code, amount, at, owner);
CREATE INDEX status_col ON status USING sift_col (code, label, is_final, rank);
