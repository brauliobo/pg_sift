-- The shredder against plain SQL on random documents (docs/COL-SHRED.md): a plpgsql oracle built on jsonb_each /
-- jsonb_array_elements / jsonb_typeof and the jsonb casts decides every slot and list, for clean and for dirty documents (wrong
-- types, wrong shapes, out of range numbers, long text); clean documents are also compared with jsonb_to_recordset,
-- jsonb_path_query and SQL aggregates, and the kernel summaries with SQL aggregates of the slots.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT setseed(0.4242);
\set ndocs 1500
\set spec 'attributes->locale text, s text, i2 int2, i4 int4, i8 int8, f4 float4, f8 float8, b bool, metrics.* float4, items[]{category text, kind text, sku text, qty int4, price float4}, tags[] text, matrix[][] float8, o[]{id int4, l[]{v text}}'

-- ---------- the generator ----------
CREATE FUNCTION rint(lo bigint, hi bigint) RETURNS bigint LANGUAGE sql AS 'SELECT lo + floor(random() * (hi - lo + 1))::bigint';
CREATE FUNCTION pick(a text[]) RETURNS text LANGUAGE sql AS 'SELECT a[rint(1, array_length(a, 1))]';

-- a value for a column of the given type: absent (SQL NULL), JSON null, wrong type or shape (dirty), out of range (dirty), or fine
CREATE FUNCTION gv(typ text, clean bool) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
	r float8 := random();
	p_absent float8 := CASE WHEN clean THEN 0.12 ELSE 0.08 END;
	p_null float8 := CASE WHEN clean THEN 0.20 ELSE 0.15 END;
BEGIN
	IF r < p_absent THEN RETURN NULL; END IF;
	IF r < p_null THEN RETURN 'null'::jsonb; END IF;
	IF NOT clean AND r < 0.25 THEN
		RETURN pick(ARRAY['"7"', 'true', '12', '"x"', '[]', '[1]', '{}', '{"a":1}', '1.5'])::jsonb;
	END IF;
	IF typ = 'cat' THEN RETURN to_jsonb(pick(ARRAY['x', 'y', 'z']));
	ELSIF typ = 'kind' THEN RETURN to_jsonb(pick(ARRAY['p', 'q']));
	ELSIF typ = 'text' THEN
		RETURN to_jsonb(pick(ARRAY['a', 'b', 'x', 'p', 'q', 'héllo', '日本語', '😀 emoji', '', ' sp ace', E'tab\t', 'quo"te', 'back\slash', 'e' || chr(769)]) ||
						CASE WHEN NOT clean AND random() < 0.03 THEN repeat('z', 300) ELSE '' END);
	ELSIF typ = 'bool' THEN
		RETURN to_jsonb(random() < 0.5);
	ELSIF clean THEN
		RETURN CASE WHEN typ LIKE 'int%' OR random() < 0.4 THEN to_jsonb(rint(-1000, 1000))
					ELSE to_jsonb(round((random() * 2000 - 1000)::numeric, rint(0, 3)::int)) END;
	END IF;
	RETURN CASE WHEN random() < 0.5
		THEN pick(ARRAY['0', '-0', '1', '-1', '3.0', '3.5', '1e2', '2147483647', '2147483648', '-2147483648', '-2147483649', '32767', '32768', '-32768',
						'-32769', '9223372036854775807', '9223372036854775808', '-9223372036854775808', '-9223372036854775809', '12345678901234567890',
						'1e400', '1e-400', '1e-45', '1e-46', '1e-40', '3.4028235e38', '3.5e38', '16777217', '0.1', '0.30000000000000004',
						'123456789.123456789', '1.7976931348623157e308', '5e-324', '1e309', '9007199254740993', '0.000000000000000000000000000000000001'])::jsonb
		ELSE to_jsonb((random() * 10 ^ rint(0, 12) - 10 ^ rint(0, 6))::numeric(40, 12)) END;
END $$;

CREATE FUNCTION el(j jsonb) RETURNS jsonb LANGUAGE sql AS 'SELECT coalesce(j, ''null''::jsonb)';
-- an object of the pairs key, value, ..., leaving out the absent (NULL) values
CREATE FUNCTION obj(VARIADIC kv jsonb[]) RETURNS jsonb LANGUAGE sql AS
$$ SELECT coalesce(jsonb_object_agg(kv[i] #>> '{}', kv[i + 1]), '{}') FROM generate_series(1, array_length(kv, 1), 2) i WHERE kv[i + 1] IS NOT NULL $$;
-- an odd value in place of a container, for dirty documents
CREATE FUNCTION odd(j jsonb, clean bool) RETURNS jsonb LANGUAGE sql AS
$$ SELECT CASE WHEN clean OR random() > 0.12 THEN j ELSE pick(ARRAY['null', '5', '"s"', '[]', '{}', '[1,2]', '{"k":1}'])::jsonb END $$;
-- members no path declares; an object with many of them is searched key by key instead of walked
CREATE FUNCTION noise(n int) RETURNS jsonb LANGUAGE sql AS
$$ SELECT coalesce(jsonb_object_agg('z' || j, CASE j % 4 WHEN 0 THEN to_jsonb(j) WHEN 1 THEN to_jsonb('s' || j)
							WHEN 2 THEN jsonb_build_object('items', j, 'k', jsonb_build_array(j)) ELSE jsonb_build_array(j, 'x') END), '{}') FROM generate_series(1, n) j $$;
CREATE FUNCTION arr_of(n int, kind text, clean bool) RETURNS jsonb LANGUAGE sql AS
$$ SELECT coalesce(jsonb_agg(CASE kind
		WHEN 'text' THEN el(gv('text', clean))
		WHEN 'float8' THEN el(gv('float8', clean))
		WHEN 'item' THEN odd(obj('"category"', gv('cat', clean), '"kind"', gv('kind', clean), '"sku"', gv('text', clean),
								 '"qty"', gv('int4', clean), '"price"', gv('float4', clean)) || CASE WHEN random() < 0.2 THEN noise(20) ELSE '{}' END, clean)
		WHEN 'leaf' THEN odd(obj('"v"', gv('text', clean)), clean)
		END), '[]') FROM generate_series(1, n) $$;

CREATE FUNCTION gen(clean bool) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE m jsonb := '{}'; matrix jsonb; o jsonb; k text;
BEGIN
	IF NOT clean AND random() < 0.02 THEN RETURN pick(ARRAY['5', '"x"', 'null', '[]', '[{"s":"a"}]', 'true'])::jsonb; END IF;
	FOR k IN SELECT DISTINCT pick(ARRAY['cpu', 'io', 'net', 'mem', 'disk', 'latency', 'x', 'é']) FROM generate_series(1, rint(0, 6)::int) LOOP
		m := m || obj(to_jsonb(k), gv('float4', clean));
	END LOOP;
	SELECT coalesce(jsonb_agg(odd(arr_of(rint(0, 4)::int, 'float8', clean), clean)), '[]') INTO matrix FROM generate_series(1, rint(0, 3)::int);
	SELECT coalesce(jsonb_agg(odd(obj('"id"', gv('int4', clean), '"l"', odd(arr_of(rint(0, 3)::int, 'leaf', clean), clean)), clean)), '[]') INTO o FROM generate_series(1, rint(0, 3)::int);
	RETURN CASE WHEN random() < 0.3 THEN noise(rint(40, 70)::int) ELSE '{}' END || obj('"attributes"', odd(obj('"locale"', gv('text', clean)), clean),
			   '"s"', gv('text', clean), '"i2"', gv('int2', clean), '"i4"', gv('int4', clean), '"i8"', gv('int8', clean),
			   '"f4"', gv('float4', clean), '"f8"', gv('float8', clean), '"b"', gv('bool', clean),
			   '"metrics"', odd(m, clean), '"items"', odd(arr_of(rint(0, 6)::int, 'item', clean), clean),
			   '"tags"', odd(arr_of(rint(0, 5)::int, 'text', clean), clean), '"matrix"', odd(matrix, clean), '"o"', odd(o, clean));
END $$;

CREATE TABLE docs (id int PRIMARY KEY, clean bool, doc jsonb);
INSERT INTO docs SELECT i, i % 3 = 0, gen(i % 3 = 0) FROM generate_series(1, :ndocs) i;
SELECT count(*) AS docs, count(*) FILTER (WHERE clean) AS clean_docs,
	   count(*) FILTER (WHERE jsonb_typeof(doc) <> 'object') > 0 AS some_not_objects FROM docs;

-- ---------- the oracle ----------
-- the jsonb found by walking object keys: absent on a scalar or null on the way, overflow on an array
CREATE FUNCTION walk(doc jsonb, VARIADIC steps text[], OUT res jsonb, OUT ovf bool) LANGUAGE plpgsql AS $$
DECLARE s text; cur jsonb := doc;
BEGIN
	ovf := false;
	FOREACH s IN ARRAY steps LOOP
		CASE jsonb_typeof(cur)
			WHEN 'object' THEN
				cur := cur -> s;
				IF cur IS NULL THEN res := NULL; RETURN; END IF;
			WHEN 'array' THEN ovf := true; res := NULL; RETURN;
			ELSE res := NULL; RETURN;
		END CASE;
	END LOOP;
	res := cur;
END $$;

-- the state and value of a scalar: the jsonb casts decide what fits
CREATE FUNCTION fit(j jsonb, ovf bool, typ text, OUT state text, OUT val text) LANGUAGE plpgsql AS $$
BEGIN
	IF ovf THEN state := 'overflow'; RETURN; END IF;
	IF j IS NULL THEN state := 'absent'; RETURN; END IF;
	IF jsonb_typeof(j) = 'null' THEN state := 'null'; RETURN; END IF;
	state := 'overflow';
	BEGIN
		IF typ = 'text' THEN
			IF jsonb_typeof(j) = 'string' AND octet_length(j #>> '{}') <= 255 THEN val := j #>> '{}'; END IF;
		ELSIF typ = 'bool' THEN
			IF jsonb_typeof(j) = 'boolean' THEN val := j::bool::text; END IF;
		ELSIF jsonb_typeof(j) = 'number' THEN
			IF typ = 'float4' THEN val := j::float4::text;
			ELSIF typ = 'float8' THEN val := j::float8::text;
			ELSIF trunc(j::numeric) = j::numeric THEN
				val := CASE typ WHEN 'int2' THEN (j::numeric)::int2::text WHEN 'int4' THEN (j::numeric)::int4::text ELSE (j::numeric)::int8::text END;
			END IF;
		END IF;
	EXCEPTION WHEN numeric_value_out_of_range OR invalid_text_representation THEN val := NULL;
	END;
	IF val IS NOT NULL THEN state := 'value'; END IF;
END $$;

-- the state of the list under a parent: want is 'object' for a map and 'array' for a list
CREATE FUNCTION lst(j jsonb, ovf bool, want text) RETURNS text LANGUAGE sql AS
$$ SELECT CASE WHEN ovf THEN 'overflow' WHEN j IS NULL THEN 'absent' WHEN jsonb_typeof(j) = 'null' THEN 'null'
			   WHEN jsonb_typeof(j) = want THEN 'value' ELSE 'overflow' END $$;

-- the fields of an element that is an object, or the element's own shape
CREATE FUNCTION efield(e jsonb, f text, typ text, OUT state text, OUT val text) LANGUAGE plpgsql AS $$
BEGIN
	IF jsonb_typeof(e) = 'object' THEN SELECT * INTO state, val FROM fit(e -> f, false, typ);
	ELSIF jsonb_typeof(e) = 'array' THEN state := 'overflow';
	ELSE state := 'absent';
	END IF;
END $$;

CREATE FUNCTION oracle(doc jsonb) RETURNS TABLE (kind text, path text, parent int, ord int, key text, state text, val text) LANGUAGE plpgsql AS $$
DECLARE
	sc record; w record; f record; e record; x record; m record;
	n int; k int; j int; ls text;
BEGIN
	FOR sc IN SELECT * FROM (VALUES ('attributes.locale', ARRAY['attributes', 'locale'], 'text'), ('s', ARRAY['s'], 'text'), ('i2', ARRAY['i2'], 'int2'),
								('i4', ARRAY['i4'], 'int4'), ('i8', ARRAY['i8'], 'int8'), ('f4', ARRAY['f4'], 'float4'), ('f8', ARRAY['f8'], 'float8'),
								('b', ARRAY['b'], 'bool')) v(path, steps, typ) LOOP
		SELECT * INTO w FROM walk(doc, VARIADIC sc.steps);
		SELECT * INTO f FROM fit(w.res, w.ovf, sc.typ);
		RETURN QUERY SELECT 'col', sc.path, NULL::int, 0, NULL::text, f.state, f.val;
	END LOOP;

	-- metrics.*: the members of an object in jsonb order
	SELECT * INTO w FROM walk(doc, 'metrics');
	ls := lst(w.res, w.ovf, 'object'); n := 0;
	IF ls = 'value' THEN
		FOR m IN SELECT * FROM jsonb_each(w.res) LOOP
			SELECT * INTO f FROM fit(m.value, false, 'float4');
			RETURN QUERY SELECT 'col', 'metrics.*', 0, n, m.key, f.state, f.val;
			n := n + 1;
		END LOOP;
	END IF;
	RETURN QUERY SELECT 'list', 'metrics.*', 0, 0, NULL::text, ls, n::text;

	-- items[]{...}: every element is a slot, objects fill their fields
	SELECT * INTO w FROM walk(doc, 'items');
	ls := lst(w.res, w.ovf, 'array'); n := 0;
	IF ls = 'value' THEN
		FOR e IN SELECT value FROM jsonb_array_elements(w.res) LOOP
			FOR x IN SELECT * FROM (VALUES ('category', 'text'), ('kind', 'text'), ('sku', 'text'), ('qty', 'int4'), ('price', 'float4')) v(f, typ) LOOP
				SELECT * INTO f FROM efield(e.value, x.f, x.typ);
				RETURN QUERY SELECT 'col', 'items[].' || x.f, 0, n, NULL::text, f.state, f.val;
			END LOOP;
			n := n + 1;
		END LOOP;
	END IF;
	RETURN QUERY SELECT 'list', 'items[]', 0, 0, NULL::text, ls, n::text;

	-- tags[]
	SELECT * INTO w FROM walk(doc, 'tags');
	ls := lst(w.res, w.ovf, 'array'); n := 0;
	IF ls = 'value' THEN
		FOR e IN SELECT value FROM jsonb_array_elements(w.res) LOOP
			SELECT * INTO f FROM fit(e.value, false, 'text');
			RETURN QUERY SELECT 'col', 'tags[]', 0, n, NULL::text, f.state, f.val;
			n := n + 1;
		END LOOP;
	END IF;
	RETURN QUERY SELECT 'list', 'tags[]', 0, 0, NULL::text, ls, n::text;

	-- matrix[][]: a list of lists; the inner lists are numbered by the outer element
	SELECT * INTO w FROM walk(doc, 'matrix');
	ls := lst(w.res, w.ovf, 'array'); n := 0; k := 0;
	IF ls = 'value' THEN
		FOR e IN SELECT value, ordinality - 1 AS i FROM jsonb_array_elements(w.res) WITH ORDINALITY LOOP
			RETURN QUERY SELECT 'list', 'matrix[][]', e.i::int, k, NULL::text, lst(e.value, false, 'array'), CASE WHEN jsonb_typeof(e.value) = 'array' THEN jsonb_array_length(e.value) ELSE 0 END::text;
			IF jsonb_typeof(e.value) = 'array' THEN
				FOR x IN SELECT value FROM jsonb_array_elements(e.value) LOOP
					SELECT * INTO f FROM fit(x.value, false, 'float8');
					RETURN QUERY SELECT 'col', 'matrix[][]', e.i::int, k, NULL::text, f.state, f.val;
					k := k + 1;
				END LOOP;
			END IF;
			n := n + 1;
		END LOOP;
	END IF;
	RETURN QUERY SELECT 'list', 'matrix[]', 0, 0, NULL::text, ls, n::text;

	-- o[]{id, l[]{v}}
	SELECT * INTO w FROM walk(doc, 'o');
	ls := lst(w.res, w.ovf, 'array'); n := 0; k := 0;
	IF ls = 'value' THEN
		FOR e IN SELECT value, ordinality - 1 AS i FROM jsonb_array_elements(w.res) WITH ORDINALITY LOOP
			SELECT * INTO f FROM efield(e.value, 'id', 'int4');
			RETURN QUERY SELECT 'col', 'o[].id', 0, e.i::int, NULL::text, f.state, f.val;
			j := 0;
			IF jsonb_typeof(e.value) = 'object' THEN
				ls := lst(e.value -> 'l', false, 'array');
				IF ls = 'value' THEN
					FOR x IN SELECT value FROM jsonb_array_elements(e.value -> 'l') LOOP
						SELECT * INTO f FROM efield(x.value, 'v', 'text');
						RETURN QUERY SELECT 'col', 'o[].l[].v', e.i::int, k + j, NULL::text, f.state, f.val;
						j := j + 1;
					END LOOP;
				END IF;
			ELSIF jsonb_typeof(e.value) = 'array' THEN ls := 'overflow';
			ELSE ls := 'absent';
			END IF;
			RETURN QUERY SELECT 'list', 'o[].l[]', e.i::int, k, NULL::text, ls, j::text;
			k := k + j;
			n := n + 1;
		END LOOP;
		ls := 'value';
	END IF;
	RETURN QUERY SELECT 'list', 'o[]', 0, 0, NULL::text, lst(w.res, w.ovf, 'array'), n::text;
END $$;

-- ---------- slots and lists ----------
CREATE TABLE got AS
	SELECT d.id, s.kind, s.path, s.parent, s.ord, s.key, s.state, s.val
	FROM docs d, LATERAL sift._test_shred(d.doc, :'spec') s WHERE s.kind <> 'ovf';
CREATE TABLE want AS SELECT d.id, o.* FROM docs d, LATERAL oracle(d.doc) o;
-- an oracle entry for a list under a missing parent has no counterpart: the shredder numbers lists under existing parents only
DELETE FROM want w USING docs d WHERE w.id = d.id AND w.kind = 'list' AND w.path = 'o[].l[]' AND NOT EXISTS
	(SELECT 1 FROM got g WHERE g.id = w.id AND g.kind = 'list' AND g.path = w.path AND g.parent = w.parent);

SELECT count(*) AS mismatches FROM ((SELECT * FROM got EXCEPT ALL SELECT * FROM want) UNION ALL (SELECT * FROM want EXCEPT ALL SELECT * FROM got)) m;
-- the documents reach every state of every kind of column
SELECT bool_and(n > 0) AS every_state_seen FROM
	(SELECT s.state, count(*) FILTER (WHERE g.state = s.state) AS n FROM (VALUES ('absent'), ('null'), ('value'), ('overflow')) s(state), got g WHERE g.kind = 'col' GROUP BY s.state) c;
SELECT bool_and(n > 0) AS every_list_state_seen FROM
	(SELECT s.state, count(*) FILTER (WHERE g.state = s.state) AS n FROM (VALUES ('absent'), ('null'), ('value'), ('overflow')) s(state), got g WHERE g.kind = 'list' GROUP BY s.state) c;

-- the overflow flag of a column: some slot overflowed, or a list above it did
CREATE TABLE cols (path text);
INSERT INTO cols SELECT DISTINCT path FROM sift._test_shred('{}', :'spec') WHERE kind = 'col';
INSERT INTO cols VALUES ('items[].category'), ('items[].kind'), ('items[].sku'), ('items[].qty'), ('items[].price'), ('tags[]'), ('matrix[][]'), ('o[].id'),
						('o[].l[].v'), ('metrics.*') ON CONFLICT DO NOTHING;
CREATE TABLE ovf_got AS SELECT d.id, s.path FROM docs d, LATERAL sift._test_shred(d.doc, :'spec') s WHERE s.kind = 'ovf';
CREATE TABLE ovf_want AS
	SELECT id, path FROM want WHERE kind = 'col' AND state = 'overflow'
	UNION SELECT w.id, c.path FROM want w JOIN (SELECT DISTINCT path FROM cols) c ON starts_with(c.path, w.path) WHERE w.kind = 'list' AND w.state = 'overflow';
SELECT count(*) AS ovf_mismatches FROM ((SELECT * FROM ovf_got EXCEPT SELECT * FROM ovf_want) UNION ALL (SELECT * FROM ovf_want EXCEPT SELECT * FROM ovf_got)) m;

-- ---------- clean documents: jsonb_to_recordset, jsonb_path_query, aggregates ----------
CREATE TABLE piv AS
	SELECT id, ord, max(val) FILTER (WHERE path = 'items[].category') AS category, max(val) FILTER (WHERE path = 'items[].kind') AS kind,
		   max(val) FILTER (WHERE path = 'items[].sku') AS sku, max(val) FILTER (WHERE path = 'items[].qty') AS qty,
		   max(val) FILTER (WHERE path = 'items[].price') AS price
	FROM got WHERE kind = 'col' AND path LIKE 'items[].%' GROUP BY id, ord;
CREATE TABLE rec AS
	SELECT d.id, x.n - 1 AS ord, x.category, x.kind, x.sku, x.qty::text AS qty, x.price::text AS price
	FROM docs d, LATERAL ROWS FROM (jsonb_to_recordset(d.doc -> 'items') AS (category text, kind text, sku text, qty int4, price float4)) WITH ORDINALITY AS x(category, kind, sku, qty, price, n)
	WHERE d.clean AND jsonb_typeof(d.doc) = 'object';
SELECT count(*) > 100 AS clean_items, (SELECT count(*) FROM (SELECT * FROM rec EXCEPT SELECT * FROM piv WHERE id IN (SELECT id FROM docs WHERE clean)) m) AS rec_not_in_shred,
	   (SELECT count(*) FROM (SELECT * FROM piv WHERE id IN (SELECT id FROM docs WHERE clean) EXCEPT SELECT * FROM rec) m) AS shred_not_in_rec FROM rec;

-- the same element must satisfy both conditions: the shredded slots of one ordinal against jsonb_path_query
SELECT count(*) AS docs_compared,
	   count(*) FILTER (WHERE jp <> sh) AS correlated_mismatches,
	   count(*) FILTER (WHERE jp > 0) > 10 AS some_matches,
	   count(*) FILTER (WHERE across > jp) > 0 AS across_elements_differ
FROM (SELECT d.id,
			 jsonb_array_length(jsonb_path_query_array(d.doc, '$.items[*] ? (@.category == "x" && @.kind == "p")')) AS jp,
			 (SELECT count(*) FROM piv p WHERE p.id = d.id AND p.category = 'x' AND p.kind = 'p') AS sh,
			 (SELECT CASE WHEN bool_or(p.category = 'x') AND bool_or(p.kind = 'p') THEN 1 ELSE 0 END * greatest(count(*) FILTER (WHERE p.category = 'x'), count(*) FILTER (WHERE p.kind = 'p'))
			  FROM piv p WHERE p.id = d.id) AS across
	  FROM docs d WHERE d.clean AND jsonb_typeof(d.doc -> 'items') = 'array') q;

-- aggregates over the slots equal the SQL aggregates over the document values (same type, same order)
SELECT (SELECT sum(qty::int8) FROM piv WHERE id IN (SELECT id FROM docs WHERE clean)) = (SELECT sum(qty::int8) FROM rec) AS sum_int_equal,
	   (SELECT sum(price::float4 ORDER BY id, ord) FROM piv WHERE id IN (SELECT id FROM docs WHERE clean)) IS NOT DISTINCT FROM (SELECT sum(price::float4 ORDER BY id, ord) FROM rec) AS sum_float4_equal,
	   (SELECT sum(price::float8 ORDER BY id, ord) FROM piv WHERE id IN (SELECT id FROM docs WHERE clean)) IS NOT DISTINCT FROM (SELECT sum(price::float8 ORDER BY id, ord) FROM rec) AS sum_float8_equal,
	   (SELECT min(price::float4) || '/' || max(price::float4) FROM piv WHERE id IN (SELECT id FROM docs WHERE clean)) = (SELECT min(price::float4) || '/' || max(price::float4) FROM rec) AS minmax_equal;
SELECT (SELECT count(*) FROM got g WHERE g.kind = 'col' AND g.path = 'metrics.*' AND g.state = 'value' AND g.id IN (SELECT id FROM docs WHERE clean))
	 = (SELECT count(*) FROM docs d, LATERAL jsonb_path_query(d.doc, '$.metrics.*') v WHERE d.clean AND jsonb_typeof(d.doc -> 'metrics') = 'object' AND jsonb_typeof(v) = 'number') AS metrics_equal;

-- ---------- the summaries of the kernels ----------
CREATE TABLE summ AS SELECT d.id, s.* FROM docs d, LATERAL sift._test_shred_summary(d.doc, :'spec') s;
CREATE TABLE summ_want AS
	SELECT id, path, absent, nulls, vals, overflow, coalesce(min_i::text, min_8::text, min_4::float8::text) AS min, coalesce(max_i::text, max_8::text, max_4::float8::text) AS max
	FROM (SELECT d.id, c.path,
				 count(g.*) FILTER (WHERE g.state = 'absent')::int AS absent, count(g.*) FILTER (WHERE g.state = 'null')::int AS nulls,
				 count(g.*) FILTER (WHERE g.state = 'value')::int AS vals, count(g.*) FILTER (WHERE g.state = 'overflow')::int AS overflow,
				 min(CASE WHEN c.path IN ('i2', 'i4', 'i8', 'o[].id', 'items[].qty') AND g.state = 'value' THEN g.val::int8 WHEN c.path = 'b' AND g.state = 'value' THEN (g.val = 'true')::int::int8 END) AS min_i,
				 max(CASE WHEN c.path IN ('i2', 'i4', 'i8', 'o[].id', 'items[].qty') AND g.state = 'value' THEN g.val::int8 WHEN c.path = 'b' AND g.state = 'value' THEN (g.val = 'true')::int::int8 END) AS max_i,
				 min(CASE WHEN c.path IN ('f8', 'matrix[][]') AND g.state = 'value' THEN g.val::float8 END) AS min_8,
				 max(CASE WHEN c.path IN ('f8', 'matrix[][]') AND g.state = 'value' THEN g.val::float8 END) AS max_8,
				 min(CASE WHEN c.path IN ('f4', 'metrics.*', 'items[].price') AND g.state = 'value' THEN g.val::float4 END) AS min_4,
				 max(CASE WHEN c.path IN ('f4', 'metrics.*', 'items[].price') AND g.state = 'value' THEN g.val::float4 END) AS max_4
		  FROM docs d CROSS JOIN (SELECT DISTINCT path FROM cols) c LEFT JOIN got g ON g.id = d.id AND g.kind = 'col' AND g.path = c.path GROUP BY d.id, c.path) q;
SELECT count(*) AS summary_mismatches FROM ((SELECT * FROM summ EXCEPT SELECT * FROM summ_want) UNION ALL (SELECT * FROM summ_want EXCEPT SELECT * FROM summ)) m;
SELECT count(*) FILTER (WHERE min IS NOT NULL) > 1000 AS summaries_with_values FROM summ;
