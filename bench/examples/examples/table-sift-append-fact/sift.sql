-- the fact needs no sift_col index: the table is its own column store; the dimension gets one as in the other examples
CREATE INDEX dim_col ON dim USING sift_col (id, grp, state, flag, weight);
