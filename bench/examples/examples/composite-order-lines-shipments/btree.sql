CREATE INDEX order_line_sku_idx ON order_line (sku);
CREATE INDEX header_customer_idx ON header (customer_id);
CREATE INDEX shipment_carrier_idx ON shipment (carrier);
