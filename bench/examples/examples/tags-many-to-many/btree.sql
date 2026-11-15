CREATE INDEX item_tag_tag_idx ON item_tag (tag_id, item_id);
CREATE INDEX event_item_idx ON event (item_id);
CREATE INDEX tag_label_idx ON tag (label);
CREATE INDEX item_kind_idx ON item (kind);
