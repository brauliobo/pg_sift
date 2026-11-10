CREATE INDEX note_grp_idx ON note (grp_id);
CREATE INDEX note_title_fts_idx ON note USING gin (to_tsvector('simple', title));
CREATE INDEX note_body_fts_idx ON note USING gin (to_tsvector('simple', body));
CREATE INDEX grp_kind_idx ON grp (kind);
