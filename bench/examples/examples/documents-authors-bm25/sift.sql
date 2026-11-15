CREATE INDEX document_col ON document USING sift_col (author_id, published_at);
CREATE INDEX document_bm25 ON document USING sift (body);
CREATE INDEX author_col ON author USING sift_col (id, country, tier);
