CREATE INDEX view_col ON page_view USING sift_col (doc_id, seconds, at);
CREATE INDEX doc_col ON doc USING sift_col (id, kind, title, description);
