CREATE INDEX fact_col ON fact USING sift_col (dim_id, amount, at);
CREATE INDEX dim_col ON dim USING sift_col (id, grp, state, version);
