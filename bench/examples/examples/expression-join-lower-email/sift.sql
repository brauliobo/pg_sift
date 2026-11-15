CREATE INDEX contact_col ON contact USING sift_col ((lower(email)), created_at, channel);
CREATE INDEX member_col ON member USING sift_col (email_lc, id, tier, country);
