CREATE INDEX sale_col ON sale USING sift_col (day, store, amount);
CREATE INDEX calendar_col ON calendar USING sift_col (day, dow, month, quarter, is_holiday);
