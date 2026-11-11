CREATE INDEX note_col ON note USING sift_col (grp_id, at);
CREATE INDEX note_title_bm25 ON note USING sift (title);
CREATE INDEX note_body_bm25 ON note USING sift (body);
CREATE INDEX grp_col ON grp USING sift_col (id, kind, tier);
