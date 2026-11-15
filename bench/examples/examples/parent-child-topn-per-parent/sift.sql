-- both sides as sift_col leaves: the child's key, filters and measures, the parent's key, group keys and filters
CREATE INDEX child_col ON child USING sift_col (parent_id, kind, amount, at);
CREATE INDEX parent_col ON parent USING sift_col (id, grp, state, rank, opened);
