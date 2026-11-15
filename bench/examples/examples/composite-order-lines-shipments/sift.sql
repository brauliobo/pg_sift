CREATE INDEX order_line_col ON order_line USING sift_col (order_id, line_no, sku, qty, price);
CREATE INDEX shipment_col ON shipment USING sift_col (order_id, line_no, carrier, shipped_on);
CREATE INDEX header_col ON header USING sift_col (id, customer_id, status, placed_on);
