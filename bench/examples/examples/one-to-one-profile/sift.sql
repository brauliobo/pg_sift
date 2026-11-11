CREATE INDEX activity_col ON activity USING sift_col (account_id, units, at);
CREATE INDEX account_col ON account USING sift_col (id, plan, region);
CREATE INDEX profile_col ON profile USING sift_col (account_id, tier, locale, bio_len);
