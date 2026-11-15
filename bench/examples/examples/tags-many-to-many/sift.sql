CREATE INDEX event_col ON event USING sift_col (item_id, amount, at);
CREATE INDEX item_tag_col ON item_tag USING sift_col (item_id, tag_id);
CREATE INDEX tag_col ON tag USING sift_col (id, label, grp);
CREATE INDEX item_col ON item USING sift_col (id, kind, keeper_id);
