-- Pure-SQL reference implementation of sift semantics: the correctness oracle.
--
-- Library use (other tests, test/diff): \set sift_ref_lib 1 then \ir reference.sql
-- (definitions only, no output). Run alone it also executes its self-tests.
--
--   sift_ref.tokens(doc, idx)       analyzer: sift.tokenize when available, else the
--                                   built-in regexp fallback (exact for ASCII)
--   sift_ref.parse(q) / render(ast) SiftQL -> raw AST (jsonb) -> SiftQL
--   sift_ref.analyze(ast, idx)      raw AST -> analyzed AST
--   sift_ref.match(doc, q, idx)     per-document evaluation (seq-scan semantics)
--   sift_ref.load(tbl, key, ...)    snapshot a corpus into session temp tables
--   sift_ref.load_index(idx)        same, taking table, key, predicate, k1, b from an index
--   sift_ref.search(q)              every match with its exact float4 BM25 score
--   sift_ref.topk(q, k)             ranked: score DESC, ctid ASC
--   sift_ref.count(q)               count(*) semantics
--
-- Statistics (N, avgdl, df) are those of the loaded rows, which equal sift's only
-- when no indexed-but-unvacuumed versions exist (fresh build, REINDEX).
--
-- Canonical float4 evaluation (sift_kernels.h); every step is
-- rounded to float4 exactly as C float arithmetic with -ffp-contract=off:
--   idf   = (float4) log(1.0 + (N - df + 0.5) / (df + 0.5))          [double]
--   avgdl = (float4) ((double) sum_dl / N)
--   k1, b = (float4) reloption doubles
--   w     = (boost * idf) * (k1 + 1)
--   knorm = (k1 * (1 - b)) + ((k1 * b) / avgdl) * dl       [sift_kern_bm25_init, knorm]
--   s     = w * (tf / (tf + knorm))       tf <= 255, dl <= 65535
--   score = ((0 + s_1) + s_2) + ...  over scoring units in the pre-order of the
--           normalized query (sift.parse_query: duplicate leaves merged, positives
--           first), the order sift sums in: a
--           term is one unit, a phrase one unit per distinct term (first
--           occurrence order), an expansion leaf one unit = max over its
--           expansions. A unit counts when the document matches the query and
--           the unit's leaf matches the document; NOT subtrees never count.
--           Group boosts multiply into leaves, root first.
--
-- sift_ref.mutant (GUC) injects known bugs for mutation testing of the harness.

\set sift_ref_echo :ECHO
\set ECHO none
SET client_min_messages = warning;
-- bodies reference the session temp tables created by sift_ref.load()
SET check_function_bodies = off;
CREATE SCHEMA IF NOT EXISTS sift_ref;

CREATE OR REPLACE FUNCTION sift_ref.mutant(m text) RETURNS bool LANGUAGE sql STABLE
AS $$ SELECT coalesce(current_setting('sift_ref.mutant', true), '') = m $$;

CREATE OR REPLACE FUNCTION sift_ref.syntax_error(pos int, what text DEFAULT 'syntax error') RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	RAISE EXCEPTION 'sift_ref: % at position %', what, pos USING ERRCODE = 'syntax_error';
END $$;

CREATE OR REPLACE FUNCTION sift_ref.limit_error(pos int, what text) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	RAISE EXCEPTION 'sift_ref: % at position %', what, pos USING ERRCODE = 'program_limit_exceeded';
END $$;

-- Index reloption value, NULL when unset
CREATE OR REPLACE FUNCTION sift_ref.opt(idx regclass, name text) RETURNS text LANGUAGE sql STABLE
AS $$
	SELECT substr(o, length(name) + 2) FROM pg_class, unnest(reloptions) o
	WHERE oid = idx AND o LIKE name || '=%'
$$;

-- 'sift' once sift.tokenize works, else 'builtin'; SET sift_ref.tokenizer to force one
CREATE OR REPLACE FUNCTION sift_ref.tokenizer() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	t text := current_setting('sift_ref.tokenizer', true);
BEGIN
	IF t IN ('sift', 'builtin') THEN
		RETURN t;
	END IF;
	BEGIN
		PERFORM sift.tokenize('probe');
		t := 'sift';
	EXCEPTION WHEN feature_not_supported OR undefined_function OR invalid_schema_name THEN
		t := 'builtin';
	END;
	PERFORM set_config('sift_ref.tokenizer', t, false);
	RETURN t;
END $$;

-- Full Unicode case folding: PostgreSQL 18 has casefold(); before it, lower() (simple mapping, pg_c_utf8) with
-- the characters whose full folding differs from it (generated from Unicode CaseFolding against PostgreSQL 17)
DO $do$
BEGIN
	IF current_setting('server_version_num')::int >= 180000 THEN
		CREATE OR REPLACE FUNCTION sift_ref.casefold(s text) RETURNS text LANGUAGE sql IMMUTABLE AS
			$f$ SELECT casefold(s COLLATE pg_unicode_fast) $f$;
	ELSE
		DROP TABLE IF EXISTS sift_ref.casefold_map CASCADE;
		CREATE TABLE sift_ref.casefold_map AS
		SELECT chr(('x' || lpad(split_part(e, '=', 1), 8, '0'))::bit(32)::int) AS c,
			(SELECT string_agg(chr(('x' || lpad(h, 8, '0'))::bit(32)::int), '' ORDER BY n)
			 FROM unnest(string_to_array(split_part(e, '=', 2), '.')) WITH ORDINALITY u(h, n)) AS f
		FROM unnest(string_to_array(
			'B5=3BC;DF=73.73;130=69.307;149=2BC.6E;17F=73;1F0=6A.30C;345=3B9;390=3B9.308.301;3B0=3C5.308.301;3C2=3C3;'
			'3D0=3B2;3D1=3B8;3D5=3C6;3D6=3C0;3F0=3BA;3F1=3C1;3F5=3B5;587=565.582;13A0=13A0;13A1=13A1;13A2=13A2;13A3=13A3;'
			'13A4=13A4;13A5=13A5;13A6=13A6;13A7=13A7;13A8=13A8;13A9=13A9;13AA=13AA;13AB=13AB;13AC=13AC;13AD=13AD;13AE=13AE;'
			'13AF=13AF;13B0=13B0;13B1=13B1;13B2=13B2;13B3=13B3;13B4=13B4;13B5=13B5;13B6=13B6;13B7=13B7;13B8=13B8;13B9=13B9;'
			'13BA=13BA;13BB=13BB;13BC=13BC;13BD=13BD;13BE=13BE;13BF=13BF;13C0=13C0;13C1=13C1;13C2=13C2;13C3=13C3;13C4=13C4;'
			'13C5=13C5;13C6=13C6;13C7=13C7;13C8=13C8;13C9=13C9;13CA=13CA;13CB=13CB;13CC=13CC;13CD=13CD;13CE=13CE;13CF=13CF;'
			'13D0=13D0;13D1=13D1;13D2=13D2;13D3=13D3;13D4=13D4;13D5=13D5;13D6=13D6;13D7=13D7;13D8=13D8;13D9=13D9;13DA=13DA;'
			'13DB=13DB;13DC=13DC;13DD=13DD;13DE=13DE;13DF=13DF;13E0=13E0;13E1=13E1;13E2=13E2;13E3=13E3;13E4=13E4;13E5=13E5;'
			'13E6=13E6;13E7=13E7;13E8=13E8;13E9=13E9;13EA=13EA;13EB=13EB;13EC=13EC;13ED=13ED;13EE=13EE;13EF=13EF;13F0=13F0;'
			'13F1=13F1;13F2=13F2;13F3=13F3;13F4=13F4;13F5=13F5;13F8=13F0;13F9=13F1;13FA=13F2;13FB=13F3;13FC=13F4;13FD=13F5;'
			'1C80=432;1C81=434;1C82=43E;1C83=441;1C84=442;1C85=442;1C86=44A;1C87=463;1C88=A64B;1C89=1C8A;1E96=68.331;'
			'1E97=74.308;1E98=77.30A;1E99=79.30A;1E9A=61.2BE;1E9B=1E61;1E9E=73.73;1F50=3C5.313;1F52=3C5.313.300;'
			'1F54=3C5.313.301;1F56=3C5.313.342;1F80=1F00.3B9;1F81=1F01.3B9;1F82=1F02.3B9;1F83=1F03.3B9;1F84=1F04.3B9;'
			'1F85=1F05.3B9;1F86=1F06.3B9;1F87=1F07.3B9;1F88=1F00.3B9;1F89=1F01.3B9;1F8A=1F02.3B9;1F8B=1F03.3B9;'
			'1F8C=1F04.3B9;1F8D=1F05.3B9;1F8E=1F06.3B9;1F8F=1F07.3B9;1F90=1F20.3B9;1F91=1F21.3B9;1F92=1F22.3B9;'
			'1F93=1F23.3B9;1F94=1F24.3B9;1F95=1F25.3B9;1F96=1F26.3B9;1F97=1F27.3B9;1F98=1F20.3B9;1F99=1F21.3B9;'
			'1F9A=1F22.3B9;1F9B=1F23.3B9;1F9C=1F24.3B9;1F9D=1F25.3B9;1F9E=1F26.3B9;1F9F=1F27.3B9;1FA0=1F60.3B9;'
			'1FA1=1F61.3B9;1FA2=1F62.3B9;1FA3=1F63.3B9;1FA4=1F64.3B9;1FA5=1F65.3B9;1FA6=1F66.3B9;1FA7=1F67.3B9;'
			'1FA8=1F60.3B9;1FA9=1F61.3B9;1FAA=1F62.3B9;1FAB=1F63.3B9;1FAC=1F64.3B9;1FAD=1F65.3B9;1FAE=1F66.3B9;'
			'1FAF=1F67.3B9;1FB2=1F70.3B9;1FB3=3B1.3B9;1FB4=3AC.3B9;1FB6=3B1.342;1FB7=3B1.342.3B9;1FBC=3B1.3B9;1FBE=3B9;'
			'1FC2=1F74.3B9;1FC3=3B7.3B9;1FC4=3AE.3B9;1FC6=3B7.342;1FC7=3B7.342.3B9;1FCC=3B7.3B9;1FD2=3B9.308.300;'
			'1FD3=3B9.308.301;1FD6=3B9.342;1FD7=3B9.308.342;1FE2=3C5.308.300;1FE3=3C5.308.301;1FE4=3C1.313;1FE6=3C5.342;'
			'1FE7=3C5.308.342;1FF2=1F7C.3B9;1FF3=3C9.3B9;1FF4=3CE.3B9;1FF6=3C9.342;1FF7=3C9.342.3B9;1FFC=3C9.3B9;A7CB=264;'
			'A7CC=A7CD;A7DA=A7DB;A7DC=19B;AB70=13A0;AB71=13A1;AB72=13A2;AB73=13A3;AB74=13A4;AB75=13A5;AB76=13A6;AB77=13A7;'
			'AB78=13A8;AB79=13A9;AB7A=13AA;AB7B=13AB;AB7C=13AC;AB7D=13AD;AB7E=13AE;AB7F=13AF;AB80=13B0;AB81=13B1;AB82=13B2;'
			'AB83=13B3;AB84=13B4;AB85=13B5;AB86=13B6;AB87=13B7;AB88=13B8;AB89=13B9;AB8A=13BA;AB8B=13BB;AB8C=13BC;AB8D=13BD;'
			'AB8E=13BE;AB8F=13BF;AB90=13C0;AB91=13C1;AB92=13C2;AB93=13C3;AB94=13C4;AB95=13C5;AB96=13C6;AB97=13C7;AB98=13C8;'
			'AB99=13C9;AB9A=13CA;AB9B=13CB;AB9C=13CC;AB9D=13CD;AB9E=13CE;AB9F=13CF;ABA0=13D0;ABA1=13D1;ABA2=13D2;ABA3=13D3;'
			'ABA4=13D4;ABA5=13D5;ABA6=13D6;ABA7=13D7;ABA8=13D8;ABA9=13D9;ABAA=13DA;ABAB=13DB;ABAC=13DC;ABAD=13DD;ABAE=13DE;'
			'ABAF=13DF;ABB0=13E0;ABB1=13E1;ABB2=13E2;ABB3=13E3;ABB4=13E4;ABB5=13E5;ABB6=13E6;ABB7=13E7;ABB8=13E8;ABB9=13E9;'
			'ABBA=13EA;ABBB=13EB;ABBC=13EC;ABBD=13ED;ABBE=13EE;ABBF=13EF;FB00=66.66;FB01=66.69;FB02=66.6C;FB03=66.66.69;'
			'FB04=66.66.6C;FB05=73.74;FB06=73.74;FB13=574.576;FB14=574.565;FB15=574.56B;FB16=57E.576;FB17=574.56D;'
			'10D50=10D70;10D51=10D71;10D52=10D72;10D53=10D73;10D54=10D74;10D55=10D75;10D56=10D76;10D57=10D77;10D58=10D78;'
			'10D59=10D79;10D5A=10D7A;10D5B=10D7B;10D5C=10D7C;10D5D=10D7D;10D5E=10D7E;10D5F=10D7F;10D60=10D80;10D61=10D81;'
			'10D62=10D82;10D63=10D83;10D64=10D84;10D65=10D85;'
			, ';')) e WHERE e <> '';
		ALTER TABLE sift_ref.casefold_map ADD PRIMARY KEY (c);
		CREATE OR REPLACE FUNCTION sift_ref.casefold(s text) RETURNS text LANGUAGE sql STABLE AS
			$f$ SELECT CASE WHEN s IS NULL THEN NULL ELSE coalesce((SELECT string_agg(coalesce(m.f, lower(r.ch COLLATE pg_c_utf8)), '' ORDER BY r.n)
				FROM regexp_split_to_table(s, '') WITH ORDINALITY r(ch, n) LEFT JOIN sift_ref.casefold_map m ON m.c = r.ch), '') END $f$;
	END IF;
END $do$;

-- Case fold (full Unicode) and accent strip (NFKD minus combining marks)
CREATE OR REPLACE FUNCTION sift_ref.fold(s text, idx regclass DEFAULT NULL) RETURNS text
LANGUAGE sql STABLE AS $$
	SELECT CASE WHEN coalesce(sift_ref.opt(idx, 'accent_folding'), 'true') IN ('true', 'on')
		THEN regexp_replace(normalize(c, NFKD), '[̀-ͯ᪰-᫿᷀-᷿⃐-⃿︠-︯]', '', 'g')
		ELSE c END
	FROM (SELECT CASE WHEN coalesce(sift_ref.opt(idx, 'case_folding'), 'true') IN ('true', 'on')
		THEN sift_ref.casefold(s) ELSE s END AS c) x
$$;

-- Longest character prefix of t that fits 255 bytes
CREATE OR REPLACE FUNCTION sift_ref.trunc_term(t text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN octet_length(t) <= 255 THEN t ELSE
		(SELECT left(t, k) FROM generate_series(255, 1, -1) k WHERE octet_length(left(t, k)) <= 255 LIMIT 1) END
$$;

-- Token stream (array index = position + 1); query = true marks query-side analysis
CREATE OR REPLACE FUNCTION sift_ref.tokens(doc text, idx regclass DEFAULT NULL, query bool DEFAULT false)
RETURNS text[] LANGUAGE plpgsql STABLE AS $$
DECLARE
	stem text := nullif(sift_ref.opt(idx, 'stemmer'), '');
	toks text[];
BEGIN
	IF doc IS NULL THEN
		RETURN NULL;
	END IF;
	IF sift_ref.tokenizer() = 'sift' AND NOT (query AND sift_ref.mutant('nofold_query')) THEN
		SELECT coalesce(array_agg(token ORDER BY pos), '{}') INTO toks FROM sift.tokenize(doc, idx);
		RETURN toks;
	END IF;
	SELECT coalesce(array_agg(sift_ref.trunc_term(m[1]) ORDER BY n), '{}') INTO toks
	FROM regexp_matches(CASE WHEN query AND sift_ref.mutant('nofold_query') THEN doc ELSE sift_ref.fold(doc, idx) END
			COLLATE pg_c_utf8, '([[:alnum:]]+|[\U0001F000-\U0001FAFF☀-➿])', 'g')
		WITH ORDINALITY AS r(m, n);
	IF stem IS NOT NULL THEN
		SELECT array_agg(coalesce((ts_lexize(stem::regdictionary, t))[1], t) ORDER BY n) INTO toks
		FROM unnest(toks) WITH ORDINALITY AS u(t, n);
	END IF;
	RETURN toks;
END $$;

-- SiftQL lexer: jsonb array of {t, v, pos, sp}; words also carry lits/metas
-- (literal fragments around unescaped * and ?)
CREATE OR REPLACE FUNCTION sift_ref.lex(q text) RETURNS jsonb LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	cs text[] := regexp_split_to_array(q, '');
	n int := length(q);
	i int := 1;
	j int;
	c text;
	sp bool := true;
	res jsonb := '[]';
	v text;
	lit text;
	lits text[];
	metas text[];
BEGIN
	WHILE i <= n LOOP
		c := cs[i];
		j := i;
		IF c ~ '^\s$' THEN
			i := i + 1;
			sp := true;
			CONTINUE;
		ELSIF c IN ('(', ')', '{', '}', '~') THEN
			res := res || jsonb_build_object('t', c, 'pos', i, 'sp', sp);
			i := i + 1;
		ELSIF c = '^' THEN
			i := i + 1;
			WHILE i <= n AND cs[i] ~ '^[0-9.]$' LOOP
				i := i + 1;
			END LOOP;
			v := array_to_string(cs[j + 1:i - 1], '');
			IF v !~ '^[0-9]+(\.[0-9]+)?$' THEN
				PERFORM sift_ref.syntax_error(j);
			END IF;
			res := res || jsonb_build_object('t', c, 'v', v, 'pos', j, 'sp', sp);
		ELSIF c IN ('"', '/') THEN
			v := '';
			i := i + 1;
			WHILE i <= n AND cs[i] <> c LOOP
				IF cs[i] = '\' AND i < n THEN
					i := i + 1;
					-- regex keeps its escapes, except the escaped delimiter
					v := v || CASE WHEN c = '/' AND cs[i] <> '/' THEN '\' ELSE '' END;
				END IF;
				v := v || cs[i];
				i := i + 1;
			END LOOP;
			IF i > n THEN
				PERFORM sift_ref.syntax_error(j, 'unterminated ' || c);
			END IF;
			i := i + 1;
			res := res || jsonb_build_object('t', c, 'v', v, 'pos', j, 'sp', sp);
		ELSIF c = ';' THEN
			PERFORM sift_ref.syntax_error(i);
		ELSE
			lit := '';
			lits := '{}';
			metas := '{}';
			WHILE i <= n AND cs[i] !~ '^[\s(){}"^~/;]$' LOOP
				IF cs[i] = '\' THEN
					IF i = n THEN
						PERFORM sift_ref.syntax_error(i, 'dangling escape');
					END IF;
					i := i + 1;
					lit := lit || cs[i];
				ELSIF cs[i] IN ('*', '?') THEN
					lits := lits || lit;
					metas := metas || cs[i];
					lit := '';
				ELSE
					lit := lit || cs[i];
				END IF;
				i := i + 1;
			END LOOP;
			v := array_to_string(cs[j:i - 1], '');
			res := res || jsonb_build_object('t', CASE WHEN v IN ('AND', 'OR', 'NOT') THEN 'kw' ELSE 'w' END,
				'v', v, 'lits', to_jsonb(lits || lit), 'metas', to_jsonb(metas), 'pos', j, 'sp', sp);
		END IF;
		sp := false;
	END LOOP;
	RETURN res || jsonb_build_object('t', 'eof', 'pos', n + 1, 'sp', true);
END $$;

-- Recursive descent over lex() output; i is the 1-based token cursor, d the nesting depth
CREATE OR REPLACE FUNCTION sift_ref.p_atom(toks jsonb, INOUT i int, OUT node jsonb) LANGUAGE plpgsql AS $$
DECLARE
	t jsonb := toks -> (i - 1);
	nx jsonb;
BEGIN
	i := i + 1;
	IF t ->> 't' = '/' THEN
		node := jsonb_build_object('op', 'regex', 'text', t -> 'v', 'pos', t -> 'pos');
		RETURN;
	END IF;
	node := jsonb_build_object('op', CASE
			WHEN jsonb_array_length(t -> 'metas') = 0 THEN 'word'
			WHEN t -> 'metas' = '["*"]' AND t -> 'lits' ->> 1 = '' THEN 'prefix'
			ELSE 'wildcard' END,
		'text', t -> 'lits' -> 0, 'lits', t -> 'lits', 'metas', t -> 'metas', 'src', t -> 'v', 'pos', t -> 'pos');
	nx := toks -> (i - 1);
	IF nx ->> 't' = '~' AND NOT (nx ->> 'sp')::bool THEN
		IF node ->> 'op' <> 'word' THEN
			PERFORM sift_ref.syntax_error((nx ->> 'pos')::int);
		END IF;
		i := i + 1;
		node := node || '{"op": "fuzzy", "dist": 1}';
		nx := toks -> (i - 1);
		IF nx ->> 't' = 'w' AND NOT (nx ->> 'sp')::bool AND nx ->> 'v' IN ('1', '2') THEN
			node := node || jsonb_build_object('dist', (nx ->> 'v')::int);
			i := i + 1;
		END IF;
	END IF;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.p_or(toks jsonb, INOUT i int, d int, OUT node jsonb) LANGUAGE plpgsql AS $$
DECLARE
	args jsonb := '[]';
	n jsonb;
BEGIN
	IF d > 32 THEN
		PERFORM sift_ref.limit_error((toks -> (i - 1) ->> 'pos')::int, 'nesting deeper than 32');
	END IF;
	LOOP
		SELECT * INTO i, n FROM sift_ref.p_and(toks, i, d);
		args := args || jsonb_build_array(n);
		EXIT WHEN toks -> (i - 1) ->> 'v' IS DISTINCT FROM 'OR' OR toks -> (i - 1) ->> 't' <> 'kw';
		i := i + 1;
	END LOOP;
	node := CASE WHEN jsonb_array_length(args) = 1 THEN args -> 0 ELSE jsonb_build_object('op', 'or', 'args', args) END;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.p_and(toks jsonb, INOUT i int, d int, OUT node jsonb) LANGUAGE plpgsql AS $$
DECLARE
	args jsonb := '[]';
	n jsonb;
	t jsonb;
BEGIN
	LOOP
		SELECT * INTO i, n FROM sift_ref.p_unary(toks, i, d);
		args := args || jsonb_build_array(n);
		t := toks -> (i - 1);
		IF t ->> 't' = 'kw' AND t ->> 'v' = 'AND' THEN
			i := i + 1;
		ELSIF NOT (t ->> 't' IN ('w', '"', '/', '(', '{') OR t ->> 't' = 'kw' AND t ->> 'v' = 'NOT') THEN
			EXIT;
		END IF;
	END LOOP;
	node := CASE WHEN jsonb_array_length(args) = 1 THEN args -> 0 ELSE jsonb_build_object('op', 'and', 'args', args) END;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.p_unary(toks jsonb, INOUT i int, d int, OUT node jsonb) LANGUAGE plpgsql AS $$
DECLARE
	t jsonb := toks -> (i - 1);
	args jsonb := '[]';
	n jsonb;
BEGIN
	IF t ->> 't' = 'kw' AND t ->> 'v' = 'NOT' THEN
		SELECT * INTO i, n FROM sift_ref.p_unary(toks, i + 1, d);
		node := jsonb_build_object('op', 'not', 'args', jsonb_build_array(n), 'pos', t -> 'pos');
		RETURN;
	END IF;
	i := i + 1;
	CASE t ->> 't'
		WHEN '(' THEN
			SELECT * INTO i, node FROM sift_ref.p_or(toks, i, d + 1);
			IF toks -> (i - 1) ->> 't' <> ')' THEN
				PERFORM sift_ref.syntax_error((toks -> (i - 1) ->> 'pos')::int);
			END IF;
			i := i + 1;
		WHEN '{' THEN
			WHILE toks -> (i - 1) ->> 't' IN ('w', '/') LOOP
				SELECT * INTO i, n FROM sift_ref.p_atom(toks, i);
				args := args || jsonb_build_array(n);
			END LOOP;
			IF toks -> (i - 1) ->> 't' <> '}' OR args = '[]' THEN
				PERFORM sift_ref.syntax_error((toks -> (i - 1) ->> 'pos')::int);
			END IF;
			i := i + 1;
			node := jsonb_build_object('op', 'any', 'args', args);
		WHEN '"' THEN
			node := jsonb_build_object('op', 'phrase', 'text', t -> 'v', 'pos', t -> 'pos');
		WHEN 'w', '/' THEN
			SELECT * INTO i, node FROM sift_ref.p_atom(toks, i - 1);
		ELSE
			PERFORM sift_ref.syntax_error((t ->> 'pos')::int);
	END CASE;
	t := toks -> (i - 1);
	IF t ->> 't' = '^' THEN
		IF t ->> 'v' !~ '^0*[0-9]{1,5}(\.[0-9]+)?$' OR (t ->> 'v')::numeric > 10000 THEN
			PERFORM sift_ref.limit_error((t ->> 'pos')::int, 'boost out of range [0, 10000]');
		END IF;
		node := node || jsonb_build_object('boost', t ->> 'v');
		i := i + 1;
	END IF;
END $$;

-- Flattens unboosted AND-in-AND and OR-in-OR (parenthesized groups)
CREATE OR REPLACE FUNCTION sift_ref.flatten(node jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN node ->> 'op' IN ('and', 'or', 'not', 'any') THEN node || jsonb_build_object('args',
		(SELECT jsonb_agg(c ORDER BY n, m) FROM (
			SELECT n, m, c FROM jsonb_array_elements(node -> 'args') WITH ORDINALITY a(x, n),
				LATERAL (SELECT sift_ref.flatten(x) AS f) y,
				LATERAL (SELECT c, m FROM jsonb_array_elements(
					CASE WHEN node ->> 'op' IN ('and', 'or') AND f ->> 'op' = node ->> 'op' AND NOT f ? 'boost'
					THEN f -> 'args' ELSE jsonb_build_array(f) END) WITH ORDINALITY b(c, m)) z) s))
	ELSE node END
$$;

-- A NOT must be a child of an AND that has a positive child
CREATE OR REPLACE FUNCTION sift_ref.check_not(node jsonb, allowed bool DEFAULT false) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
	c jsonb;
	pos_sibling bool := node ->> 'op' = 'and' AND EXISTS (
		SELECT 1 FROM jsonb_array_elements(node -> 'args') x WHERE x ->> 'op' <> 'not');
BEGIN
	IF node ->> 'op' = 'not' AND NOT allowed THEN
		PERFORM sift_ref.syntax_error((node ->> 'pos')::int, 'NOT without a positive AND sibling');
	END IF;
	FOR c IN SELECT jsonb_array_elements(coalesce(node -> 'args', '[]')) LOOP
		PERFORM sift_ref.check_not(c, pos_sibling AND c ->> 'op' = 'not');
	END LOOP;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.parse(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
	toks jsonb := sift_ref.lex(q);
	i int;
	node jsonb;
	nleaves int;
BEGIN
	SELECT * INTO i, node FROM sift_ref.p_or(toks, 1, 1);
	IF toks -> (i - 1) ->> 't' <> 'eof' THEN
		PERFORM sift_ref.syntax_error((toks -> (i - 1) ->> 'pos')::int);
	END IF;
	node := sift_ref.flatten(node);
	PERFORM sift_ref.check_not(node);
	SELECT count(*) INTO nleaves FROM jsonb_path_query(node, 'strict $.**.op') o
	WHERE o #>> '{}' NOT IN ('and', 'or', 'not', 'any');
	IF nleaves > 256 THEN
		PERFORM sift_ref.limit_error(1, 'more than 256 leaves');
	END IF;
	RETURN node;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.quote(s text, special text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT regexp_replace(s, '([' || special || '\\])', '\\\1', 'g')
$$;

-- Raw AST -> SiftQL; juxt renders AND as juxtaposition
CREATE OR REPLACE FUNCTION sift_ref.render(node jsonb, juxt bool DEFAULT false) RETURNS text
LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	op text := node ->> 'op';
	s text;
BEGIN
	s := CASE op
		WHEN 'and' THEN (SELECT string_agg(CASE WHEN c ->> 'op' IN ('or', 'and') AND NOT c ? 'boost'
				THEN '(' || sift_ref.render(c, juxt) || ')' ELSE sift_ref.render(c, juxt) END,
				CASE WHEN juxt THEN ' ' ELSE ' AND ' END ORDER BY n)
			FROM jsonb_array_elements(node -> 'args') WITH ORDINALITY a(c, n))
		WHEN 'or' THEN (SELECT string_agg(CASE WHEN c ->> 'op' = 'or' AND NOT c ? 'boost'
				THEN '(' || sift_ref.render(c, juxt) || ')' ELSE sift_ref.render(c, juxt) END, ' OR ' ORDER BY n)
			FROM jsonb_array_elements(node -> 'args') WITH ORDINALITY a(c, n))
		WHEN 'any' THEN '{' || (SELECT string_agg(sift_ref.render(c, juxt), ' ' ORDER BY n)
			FROM jsonb_array_elements(node -> 'args') WITH ORDINALITY a(c, n)) || '}'
		WHEN 'not' THEN 'NOT ' || (SELECT CASE WHEN c ->> 'op' IN ('and', 'or') AND NOT c ? 'boost'
				THEN '(' || sift_ref.render(c, juxt) || ')' ELSE sift_ref.render(c, juxt) END
			FROM (SELECT node -> 'args' -> 0 AS c) x)
		WHEN 'phrase' THEN '"' || sift_ref.quote(node ->> 'text', '"') || '"'
		WHEN 'regex' THEN '/' || replace(node ->> 'text', '/', '\/') || '/'
		WHEN 'fuzzy' THEN (node ->> 'src') || '~' || (node ->> 'dist')
		ELSE node ->> 'src' END;
	IF node ? 'boost' THEN
		s := CASE WHEN op IN ('and', 'or', 'not') THEN '(' || s || ')' ELSE s END || '^' || (node ->> 'boost');
	END IF;
	RETURN s;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.re_escape(s text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT regexp_replace(s, '([.^$*+?()\[\]{}|\\])', '\\\1', 'g')
$$;

-- Word/phrase text -> term, phrase, or NULL (no tokens)
CREATE OR REPLACE FUNCTION sift_ref.a_text(s text, pos int, b real, idx regclass) RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE
	t text[] := sift_ref.tokens(s, idx, true);
BEGIN
	IF cardinality(t) > 32 THEN
		PERFORM sift_ref.limit_error(pos, 'phrase longer than 32 tokens');
	END IF;
	RETURN CASE cardinality(t) WHEN 0 THEN NULL
		WHEN 1 THEN jsonb_build_object('op', 'term', 'term', t[1], 'b', b)
		ELSE jsonb_build_object('op', 'phrase', 'terms', to_jsonb(t), 'b', b) END;
END $$;

-- Raw AST -> analyzed AST: term / phrase / exp(kind, re | word+dist) / and / or / not / nothing.
-- b is the effective float4 boost (ancestors' boosts multiplied root first).
CREATE OR REPLACE FUNCTION sift_ref.analyze(node jsonb, idx regclass DEFAULT NULL, eff real DEFAULT 1)
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
	op text := node ->> 'op';
	b real := eff * coalesce((node ->> 'boost')::float8::real, 1);
	pos jsonb := '[]';
	neg jsonb := '[]';
	c jsonb;
	a jsonb;
	nothing bool := false;
	re text := '';
	k int;
BEGIN
	IF op IN ('and', 'or', 'any') THEN
		FOR c IN SELECT jsonb_array_elements(node -> 'args') LOOP
			a := sift_ref.analyze(c, idx, b);
			CONTINUE WHEN a IS NULL;
			nothing := nothing OR a ->> 'op' = 'nothing';
			IF a ->> 'op' = 'not' THEN
				neg := neg || jsonb_build_array(a);
			ELSIF a ->> 'op' <> 'nothing' THEN
				pos := pos || jsonb_build_array(a);
			END IF;
		END LOOP;
	END IF;
	CASE op
		WHEN 'word', 'phrase' THEN
			RETURN sift_ref.a_text(node ->> 'text', (node ->> 'pos')::int, b, idx);
		WHEN 'prefix' THEN
			RETURN jsonb_build_object('op', 'exp', 'kind', op, 'b', b,
				're', '^' || sift_ref.re_escape(sift_ref.fold(node ->> 'text', idx)));
		WHEN 'wildcard' THEN
			FOR k IN 0 .. jsonb_array_length(node -> 'metas') LOOP
				re := re || sift_ref.re_escape(sift_ref.fold(node -> 'lits' ->> k, idx))
					|| coalesce(CASE node -> 'metas' ->> k WHEN '*' THEN '.*' WHEN '?' THEN '.' END, '');
			END LOOP;
			RETURN jsonb_build_object('op', 'exp', 'kind', op, 'b', b, 're', '^' || re || '$');
		WHEN 'regex' THEN
			re := '^(?:' || (node ->> 'text') || ')$';
			PERFORM '' ~ re;
			RETURN jsonb_build_object('op', 'exp', 'kind', op, 'b', b, 're', re);
		WHEN 'fuzzy' THEN
			RETURN jsonb_build_object('op', 'exp', 'kind', op, 'b', b,
				'word', sift_ref.fold(node ->> 'text', idx), 'dist', node -> 'dist');
		WHEN 'not' THEN
			a := sift_ref.analyze(node -> 'args' -> 0, idx, b);
			RETURN CASE WHEN a IS NULL OR a ->> 'op' = 'nothing' THEN NULL
				ELSE jsonb_build_object('op', 'not', 'args', jsonb_build_array(a)) END;
		WHEN 'and' THEN
			IF nothing THEN
				RETURN '{"op": "nothing"}';
			ELSIF pos = '[]' THEN
				RETURN CASE WHEN neg = '[]' THEN NULL ELSE '{"op": "nothing"}' END;
			ELSIF jsonb_array_length(pos) = 1 AND neg = '[]' THEN
				RETURN pos -> 0;
			END IF;
			RETURN jsonb_build_object('op', 'and', 'args', pos || neg);
		ELSE -- or, any
			IF pos = '[]' THEN
				RETURN CASE WHEN nothing THEN '{"op": "nothing"}' END;
			END IF;
			RETURN CASE WHEN jsonb_array_length(pos) = 1 THEN pos -> 0 ELSE jsonb_build_object('op', 'or', 'args', pos) END;
	END CASE;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.analyze_query(q text, idx regclass DEFAULT NULL) RETURNS jsonb
LANGUAGE sql AS $$ SELECT coalesce(sift_ref.analyze(sift_ref.parse(q), idx), '{"op": "nothing"}') $$;

-- Levenshtein distance (characters) <= d
CREATE OR REPLACE FUNCTION sift_ref.lev_le(a text, b text, d int) RETURNS bool LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	x text[] := regexp_split_to_array(a, '');
	y text[] := regexp_split_to_array(b, '');
	prev int[];
	cur int[];
	i int;
	j int;
BEGIN
	IF abs(length(a) - length(b)) > d THEN
		RETURN false;
	END IF;
	prev := array(SELECT generate_series(0, length(b)));
	FOR i IN 1 .. length(a) LOOP
		cur := ARRAY[i];
		FOR j IN 1 .. length(b) LOOP
			cur := cur || least(prev[j + 1] + 1, cur[j] + 1, prev[j] + (x[i] <> y[j])::int);
		END LOOP;
		prev := cur;
	END LOOP;
	RETURN prev[length(b) + 1] <= d;
END $$;

-- Does dictionary term t belong to expansion leaf a. Fuzzy distance is clamped
-- by the analyzed word's length in characters: 0 below 3, at most 1 below 6.
CREATE OR REPLACE FUNCTION sift_ref.exp_match(a jsonb, t text) RETURNS bool LANGUAGE plpgsql STABLE AS $$
DECLARE
	w text := a ->> 'word';
	d int := CASE WHEN length(w) < 3 THEN 0 WHEN length(w) < 6 THEN least((a ->> 'dist')::int, 1) ELSE (a ->> 'dist')::int END;
BEGIN
	IF a ->> 'kind' <> 'fuzzy' THEN
		RETURN t ~ (a ->> 're');
	ELSIF d = 0 THEN
		RETURN t = w;
	END IF;
	RETURN (left(t, 1) = left(w, 1) OR sift_ref.mutant('fuzzy_no_firstchar')) AND sift_ref.lev_le(w, t, d);
END $$;

CREATE OR REPLACE FUNCTION sift_ref.has_phrase(toks text[], terms text[]) RETURNS bool LANGUAGE sql IMMUTABLE AS $$
	SELECT EXISTS (SELECT 1 FROM unnest(array_positions(toks, terms[1])) i WHERE toks[i:i + cardinality(terms) - 1] = terms)
$$;

-- Per-document evaluation of an analyzed AST
CREATE OR REPLACE FUNCTION sift_ref.eval_doc(a jsonb, toks text[]) RETURNS bool LANGUAGE plpgsql STABLE AS $$
DECLARE
	c jsonb;
BEGIN
	CASE a ->> 'op'
		WHEN 'term' THEN
			RETURN coalesce(a ->> 'term' = ANY(toks), false);
		WHEN 'phrase' THEN
			IF sift_ref.mutant('phrase_as_and') THEN
				RETURN toks @> array(SELECT jsonb_array_elements_text(a -> 'terms'));
			END IF;
			RETURN sift_ref.has_phrase(toks, array(SELECT jsonb_array_elements_text(a -> 'terms')));
		WHEN 'exp' THEN
			RETURN EXISTS (SELECT 1 FROM unnest(toks) t WHERE sift_ref.exp_match(a, t));
		WHEN 'and' THEN
			FOR c IN SELECT jsonb_array_elements(a -> 'args') LOOP
				IF c ->> 'op' = 'not' THEN
					IF NOT sift_ref.mutant('not_ignored') AND sift_ref.eval_doc(c -> 'args' -> 0, toks) THEN
						RETURN false;
					END IF;
				ELSIF NOT sift_ref.eval_doc(c, toks) THEN
					RETURN false;
				END IF;
			END LOOP;
			RETURN true;
		WHEN 'or' THEN
			RETURN EXISTS (SELECT 1 FROM jsonb_array_elements(a -> 'args') x WHERE sift_ref.eval_doc(x, toks));
		ELSE
			RETURN false;
	END CASE;
END $$;

-- Analyzed query for per-row callers, memoized per session (key: query, index, mutant)
CREATE OR REPLACE FUNCTION sift_ref.analyzed(q text, idx regclass) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
	k text := md5(concat_ws(E'\x1f', q, idx, current_setting('sift_ref.mutant', true), sift_ref.tokenizer()));
	a jsonb;
BEGIN
	IF current_setting('sift_ref.memo_key', true) IS NOT DISTINCT FROM k THEN
		RETURN current_setting('sift_ref.memo_ast')::jsonb;
	END IF;
	a := sift_ref.analyze_query(q, idx);
	PERFORM set_config('sift_ref.memo_ast', a::text, false), set_config('sift_ref.memo_key', k, false);
	RETURN a;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.match(doc text, q text, idx regclass DEFAULT NULL) RETURNS bool
LANGUAGE sql AS $$
	SELECT CASE WHEN doc IS NOT NULL THEN sift_ref.eval_doc(sift_ref.analyzed(q, idx), sift_ref.tokens(doc, idx)) END
$$;

-- Corpus snapshot in session temp tables: sift_ref_doc(tid, toks, dl), sift_ref_tf(term, tid, tf),
-- sift_ref_df(term, df), sift_ref_stat(n, sum_dl, k1, b, idx)
CREATE OR REPLACE FUNCTION sift_ref.load(tbl regclass, key text, idx regclass DEFAULT NULL, pred text DEFAULT NULL,
	k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75) RETURNS bigint LANGUAGE plpgsql
SET client_min_messages = warning AS $$
DECLARE
	n bigint;
BEGIN
	DROP TABLE IF EXISTS pg_temp.sift_ref_doc, pg_temp.sift_ref_tf, pg_temp.sift_ref_df, pg_temp.sift_ref_stat,
		pg_temp.sift_ref_exp;
	CREATE TEMP TABLE sift_ref_exp (leaf jsonb PRIMARY KEY, terms text[]);
	EXECUTE format('CREATE TEMP TABLE sift_ref_doc AS SELECT tid, toks, least(cardinality(toks), 65535) AS dl
		FROM (SELECT ctid AS tid, sift_ref.tokens((%s)::text, %L) AS toks FROM %s WHERE %s OFFSET 0) s WHERE toks IS NOT NULL',
		key, idx, tbl, coalesce(pred, 'true'));
	CREATE TEMP TABLE sift_ref_tf AS
		SELECT t AS term, tid, count(*)::int AS tf FROM pg_temp.sift_ref_doc, unnest(toks) t GROUP BY 1, 2;
	CREATE TEMP TABLE sift_ref_df AS SELECT term, count(*) AS df FROM pg_temp.sift_ref_tf GROUP BY term;
	CREATE TEMP TABLE sift_ref_stat AS
		SELECT count(*) AS n, coalesce(sum(dl), 0) AS sum_dl, k1::real AS k1, b::real AS b, idx AS idx
		FROM pg_temp.sift_ref_doc;
	ALTER TABLE pg_temp.sift_ref_doc ADD PRIMARY KEY (tid);
	ALTER TABLE pg_temp.sift_ref_df ADD PRIMARY KEY (term);
	CREATE INDEX ON pg_temp.sift_ref_tf (term, tid);
	CREATE INDEX ON pg_temp.sift_ref_tf (tid);
	ANALYZE pg_temp.sift_ref_doc, pg_temp.sift_ref_tf, pg_temp.sift_ref_df;
	SELECT s.n INTO n FROM pg_temp.sift_ref_stat s;
	RETURN n;
END $$;

CREATE OR REPLACE FUNCTION sift_ref.load_index(idx regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT sift_ref.load(i.indrelid::regclass, pg_get_indexdef(idx, 1, true), idx,
		pg_get_expr(i.indpred, i.indrelid),
		coalesce(sift_ref.opt(idx, 'k1')::float8, 1.2), coalesce(sift_ref.opt(idx, 'b')::float8, 0.75))
	FROM pg_index i WHERE i.indexrelid = idx
$$;

CREATE OR REPLACE FUNCTION sift_ref.avgdl(sum_dl bigint, n bigint) RETURNS real LANGUAGE sql STABLE AS $$
	SELECT CASE WHEN sift_ref.mutant('avgdl_int') THEN (sum_dl / n)::real ELSE (sum_dl::float8 / n::float8)::real END
$$;

CREATE OR REPLACE FUNCTION sift_ref.stats() RETURNS TABLE (n bigint, sum_dl bigint, avgdl real, k1 real, b real)
LANGUAGE sql AS $$ SELECT n, sum_dl, sift_ref.avgdl(sum_dl, nullif(n, 0)), k1, b FROM pg_temp.sift_ref_stat $$;

CREATE OR REPLACE FUNCTION sift_ref.idf(df bigint, n bigint) RETURNS real LANGUAGE sql IMMUTABLE AS $$
	SELECT ln(1::float8 + (n::float8 - df::float8 + 0.5::float8) / (df::float8 + 0.5::float8))::real
$$;

CREATE OR REPLACE FUNCTION sift_ref.bm25(tf int, dl int, df bigint, n bigint, avgdl real, k1 real, b real, boost real)
RETURNS real LANGUAGE sql STABLE AS $$
	SELECT CASE WHEN sift_ref.mutant('float8_score')
		THEN (w::float8 * t::float8 / (t::float8 + k1::float8 * (1 - b::float8 + b::float8 * dl / avgdl::float8)))::real
		ELSE w * (t / (t + (k1 * (1::real - b) + ((k1 * b) / avgdl) * least(dl, 65535)::real))) END
	FROM (SELECT (CASE WHEN sift_ref.mutant('boost_ignored') THEN 1::real ELSE boost END
			* sift_ref.idf(df + sift_ref.mutant('idf_df_plus1')::int, n)) * (k1 + 1::real) AS w,
		least(tf, CASE WHEN sift_ref.mutant('tf_cap15') THEN 15 ELSE 255 END)::real AS t) x
$$;

-- Dictionary terms of an expansion leaf (cached per load); a leaf with more than sift.max_expansions is wide (see is_wide)
CREATE OR REPLACE FUNCTION sift_ref.expand(a jsonb) RETURNS text[] LANGUAGE plpgsql AS $$
DECLARE
	k jsonb := a - 'b' || jsonb_build_object('mutant', current_setting('sift_ref.mutant', true));
	terms text[] := (SELECT e.terms FROM pg_temp.sift_ref_exp e WHERE e.leaf = k);
BEGIN
	IF terms IS NULL THEN
		terms := array(SELECT term FROM pg_temp.sift_ref_df WHERE sift_ref.exp_match(a, term) ORDER BY term);
		INSERT INTO pg_temp.sift_ref_exp VALUES (k, terms);
	END IF;
	RETURN CASE WHEN sift_ref.mutant('prefix_trunc') THEN terms[1:4] ELSE terms END;
END $$;

-- A leaf with more terms than sift.max_expansions (the engine's) is not scored as the max over its terms but as the
-- one term prefix_index would index for a prefix: tf the occurrences of its words, df the documents that have one
CREATE OR REPLACE FUNCTION sift_ref.is_wide(a jsonb) RETURNS bool LANGUAGE sql STABLE AS $$
	SELECT cardinality(sift_ref.expand(a)) > coalesce(nullif(current_setting('sift.max_expansions', true), '')::int, 512)
$$;

-- The normalized form of q: scores are defined on it (float4 sums depend on the order)
CREATE OR REPLACE FUNCTION sift_ref.canonical(q text, idx regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text := q;
BEGIN
	IF sift_ref.tokenizer() = 'sift' THEN
		EXECUTE 'SELECT sift.parse_query($1, $2)' INTO r USING q, idx;
	END IF;
	RETURN r;
END $$;

-- Analyzed AST -> SQL yielding the matching tids of the loaded corpus
CREATE OR REPLACE FUNCTION sift_ref.compile(a jsonb) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	terms text[];
	pos text;
	neg text;
BEGIN
	CASE a ->> 'op'
		WHEN 'term' THEN
			RETURN format('SELECT tid FROM pg_temp.sift_ref_tf WHERE term = %L', a ->> 'term');
		WHEN 'phrase' THEN
			terms := array(SELECT jsonb_array_elements_text(a -> 'terms'));
			pos := (SELECT string_agg(format('(SELECT tid FROM pg_temp.sift_ref_tf WHERE term = %L)', t), ' INTERSECT ')
				FROM unnest(terms) t);
			RETURN CASE WHEN sift_ref.mutant('phrase_as_and') THEN pos
				ELSE format('SELECT d.tid FROM (%s) c JOIN pg_temp.sift_ref_doc d ON d.tid = c.tid WHERE sift_ref.has_phrase(d.toks, %L)',
					pos, terms) END;
		WHEN 'exp' THEN
			RETURN format('SELECT DISTINCT tid FROM pg_temp.sift_ref_tf WHERE term = ANY (%L::text[])', sift_ref.expand(a));
		WHEN 'and' THEN
			SELECT string_agg('(' || sift_ref.compile(c) || ')', ' INTERSECT ') FILTER (WHERE c ->> 'op' <> 'not'),
				string_agg('(' || sift_ref.compile(c -> 'args' -> 0) || ')', ' UNION ') FILTER (WHERE c ->> 'op' = 'not')
			INTO pos, neg FROM jsonb_array_elements(a -> 'args') c;
			RETURN CASE WHEN neg IS NULL OR sift_ref.mutant('not_ignored') THEN pos
				ELSE format('SELECT tid FROM (%s) p EXCEPT SELECT tid FROM (%s) n', pos, neg) END;
		WHEN 'or' THEN
			RETURN (SELECT string_agg('(' || sift_ref.compile(c) || ')', ' UNION ') FROM jsonb_array_elements(a -> 'args') c);
		ELSE
			RETURN 'SELECT NULL::tid AS tid WHERE false';
	END CASE;
END $$;

-- Scoring units of a (see header): SQL fragments yielding (tid, s) over the match set r
CREATE OR REPLACE FUNCTION sift_ref.units(a jsonb, consts text) RETURNS text[] LANGUAGE plpgsql AS $$
DECLARE
	one text := 'SELECT r.tid, %s AS s FROM r
		JOIN pg_temp.sift_ref_tf f ON f.tid = r.tid AND f.term %s
		JOIN pg_temp.sift_ref_doc d ON d.tid = r.tid JOIN pg_temp.sift_ref_df x ON x.term = f.term %s';
	s text := format('sift_ref.bm25(f.tf, d.dl, x.df, %s, %s::real)', consts, a ->> 'b');
	res text[] := '{}';
	c jsonb;
BEGIN
	CASE a ->> 'op'
		WHEN 'term' THEN
			RETURN ARRAY[format(one, s, format('= %L', a ->> 'term'), '')];
		WHEN 'phrase' THEN
			RETURN array(SELECT format(one, s, format('= %L', t), format('WHERE r.tid IN (%s)', sift_ref.compile(a)))
				FROM (SELECT t, min(n) AS n FROM jsonb_array_elements_text(a -> 'terms') WITH ORDINALITY u(t, n) GROUP BY t) p
				ORDER BY n);
		WHEN 'exp' THEN
			IF sift_ref.is_wide(a) THEN
				RETURN ARRAY[format('SELECT r.tid, sift_ref.bm25(least(f.tf, 255)::int, d.dl, u.df, %s, %s::real) AS s FROM r
					JOIN (SELECT tid, sum(tf) AS tf FROM pg_temp.sift_ref_tf WHERE term = ANY (%L::text[]) GROUP BY tid) f ON f.tid = r.tid
					JOIN pg_temp.sift_ref_doc d ON d.tid = r.tid
					CROSS JOIN (SELECT count(DISTINCT tid) AS df FROM pg_temp.sift_ref_tf WHERE term = ANY (%L::text[])) u',
					consts, a ->> 'b', sift_ref.expand(a), sift_ref.expand(a))];
			END IF;
			RETURN ARRAY[format(one, CASE WHEN sift_ref.mutant('dismax_sum') THEN 'sum(' || s || ' ORDER BY f.term)'
					ELSE 'max(' || s || ')' END,
				format('= ANY (%L::text[])', sift_ref.expand(a)), 'GROUP BY r.tid')];
		WHEN 'and', 'or' THEN
			FOR c IN SELECT jsonb_array_elements(a -> 'args') LOOP
				-- a subquery keeps it out of plpgsql's simple-expression path: recursion runs a simple expression through SPI, which
				-- trips an assertion of the read/write parameter optimization of an array variable (PostgreSQL 18, 19)
				res := res || (SELECT sift_ref.units(c, consts));
			END LOOP;
			RETURN res;
		ELSE
			RETURN res;
	END CASE;
END $$;

-- Mutants that lose postings at heap positions (codec / mask-width bugs)
CREATE OR REPLACE FUNCTION sift_ref.dropped(t tid) RETURNS bool LANGUAGE sql STABLE AS $$
	SELECT sift_ref.mutant('drop_high_offset') AND (t::text::point)[1] > 128
		OR sift_ref.mutant('drop_sb_boundary') AND (t::text::point)[0]::int % 256 = 255
$$;

CREATE OR REPLACE FUNCTION sift_ref.search(q text) RETURNS TABLE (ctid tid, score real) LANGUAGE plpgsql AS $$
DECLARE
	st record;
	a jsonb;
	u text[];
BEGIN
	SELECT s.*, sift_ref.avgdl(s.sum_dl, nullif(s.n, 0)) AS avgdl INTO st FROM pg_temp.sift_ref_stat s;
	a := sift_ref.analyze_query(sift_ref.canonical(q, st.idx), st.idx);
	u := sift_ref.units(a, format('%s, %s::real, %s::real, %s::real', st.n, st.avgdl, st.k1, st.b));
	RETURN QUERY EXECUTE format('WITH r AS MATERIALIZED (SELECT tid FROM (%s) m WHERE NOT sift_ref.dropped(tid)),
		u AS (%s)
		SELECT r.tid, coalesce(sum(u.s ORDER BY u.n), 0)::real FROM r LEFT JOIN u ON u.tid = r.tid GROUP BY r.tid',
		sift_ref.compile(a),
		coalesce((SELECT string_agg(format('SELECT %s AS n, * FROM (%s) x', n, f), ' UNION ALL ')
			FROM unnest(u) WITH ORDINALITY v(f, n)), 'SELECT 0 AS n, NULL::tid AS tid, NULL::real AS s'));
END $$;

CREATE OR REPLACE FUNCTION sift_ref.topk(q text, k int) RETURNS TABLE (rank int, ctid tid, score real)
LANGUAGE sql AS $$
	SELECT row_number() OVER (ORDER BY s.score DESC, s.ctid)::int, s.ctid, s.score
	FROM sift_ref.search(q) s
	ORDER BY s.score DESC,
		CASE WHEN sift_ref.mutant('tie_ctid_desc') THEN NULL ELSE s.ctid END,
		s.ctid DESC
	LIMIT CASE WHEN sift_ref.mutant('topk_short') THEN greatest(k - 1, 1) ELSE k END
$$;

CREATE OR REPLACE FUNCTION sift_ref.count(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM (%s) m WHERE NOT sift_ref.dropped(tid)',
		sift_ref.compile(sift_ref.analyze_query(q, (SELECT idx FROM pg_temp.sift_ref_stat)))) INTO n;
	RETURN n + (n > 0 AND sift_ref.mutant('count_plus'))::int;
END $$;

-- Reference self-consistency: compiled set evaluation == per-document evaluation
CREATE OR REPLACE FUNCTION sift_ref.selfcheck(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	a jsonb := sift_ref.analyze_query(q, (SELECT idx FROM pg_temp.sift_ref_stat));
	diff bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM (((%1$s) EXCEPT (%2$s)) UNION ALL ((%2$s) EXCEPT (%1$s))) x',
		sift_ref.compile(a), format('SELECT tid FROM pg_temp.sift_ref_doc WHERE sift_ref.eval_doc(%L, toks)', a)) INTO diff;
	RETURN diff = 0;
END $$;

RESET client_min_messages;
RESET check_function_bodies;
\set ECHO :sift_ref_echo
\if :{?sift_ref_lib}
\else
-- Self-tests (built-in analyzer, so the output does not depend on sift.tokenize)
SET sift_ref.tokenizer = builtin;
SELECT sift_ref.tokens('Héllo, WORLD! e-mail 🍕pizza 42abc Straße ÇA ﬁne');
SELECT sift_ref.tokens(''), sift_ref.tokens('  !! ... '), sift_ref.tokens(NULL);
SELECT octet_length(t[1]) AS long_token_bytes FROM sift_ref.tokens(repeat('ж', 200)) t;
-- parse -> render round trip and the analyzed form
SELECT q, sift_ref.render(sift_ref.parse(q)) AS rendered, sift_ref.analyze_query(q) AS analyzed
FROM unnest(ARRAY[
	'foo', 'foo bar', 'foo AND bar OR baz', 'a OR b c', 'NOT x foo', 'foo -bar',
	'"Quick Brown" fox^2', '(a OR b)^1.5 c', '{red green blue}', 'pre* wi?d*c', '/col(o|ou)r/',
	'fuzzy~ fuzzy~2 ab~', 'e-mail', 'foo\(bar', '\AND and', '+++ foo', '"" foo', 'a NOT +++',
	'Ünïcödé CAFÉ', 'foo^0']) q;
-- syntax and limit errors carry SQLSTATE and position
DO $$
DECLARE
	q text;
BEGIN
	FOREACH q IN ARRAY ARRAY['', '   ', 'foo AND', 'OR foo', '(foo', 'foo)', 'NOT foo', 'a OR NOT b',
		'{}', '"open', '/open', 'foo^', 'foo^10001', 'a;b', 'NOT (a b)', 'foo bar\', 'a (NOT b)', '{a "b"}'] LOOP
		BEGIN
			PERFORM sift_ref.parse(q);
			RAISE NOTICE '% -> ok', q;
		EXCEPTION WHEN OTHERS THEN
			RAISE NOTICE '% -> % %', q, SQLSTATE, SQLERRM;
		END;
	END LOOP;
END $$;
-- BM25: float4 result vs the float8 formula, and pinned bit patterns
SELECT tf, dl, df, float4send(s) AS bits, s, abs(s - f8) / f8 < 1e-6 AS close
FROM (VALUES (1, 10, 1), (3, 100, 50), (255, 65535, 999), (300, 70000, 1000), (2, 0, 7)) v(tf, dl, df),
	LATERAL (SELECT sift_ref.bm25(tf, dl, df, 1000, 12.5::real, 1.2::float8::real, 0.75::float8::real, 1.5::real) AS s,
		1.5 * ln(1 + (1000 - df + 0.5) / (df + 0.5)) * 2.2 * least(tf, 255)
			/ (least(tf, 255) + 1.2 * (0.25 + 0.75 * least(dl, 65535) / 12.5)) AS f8) x;
-- corpus: ranked search, counts, ties by ctid, and per-document agreement
CREATE TABLE ref_docs (id int, body text);
INSERT INTO ref_docs VALUES
	(1, 'the quick brown fox'), (2, 'Quick quick QUICK brown dogs'), (3, 'lazy dog sleeps'),
	(4, 'brown fox, brown fox!'), (5, ''), (6, NULL), (7, 'quick brown'), (8, 'quick brown'),
	(9, 'colour color colr'), (10, 'café CAFE cafe'), (11, repeat('spam ', 20) || 'cat cats catalog');
SELECT sift_ref.load('ref_docs', 'body');
SELECT * FROM sift_ref.stats();
SELECT q, sift_ref.count(q) AS n, (SELECT string_agg(format('%s:%s', id, score), ' ' ORDER BY rank)
	FROM sift_ref.topk(q, 4) t JOIN ref_docs d ON d.ctid = t.ctid) AS top4, sift_ref.selfcheck(q) AS self
FROM unnest(ARRAY['quick', 'quick OR fox', 'quick brown', '"brown fox"', '"fox brown"', 'brown NOT quick',
	'qu*', 'col*r', '/colou?r/', 'colr~1', 'cafe', '{dog dogs}^2 OR lazy', 'quick^0', '+++', 'zzz OR quick']) q;
SELECT d.id FROM ref_docs d WHERE sift_ref.match(d.body, 'brown NOT (dogs OR lazy)') ORDER BY 1;
-- every mutant changes some answer on this corpus
CREATE FUNCTION pg_temp.answers(m text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM set_config('sift_ref.mutant', m, false);
	SELECT string_agg(format('%s %s %s', q, sift_ref.count(q), (SELECT array_agg(t) FROM sift_ref.topk(q, 2) t)), ';')
	INTO r FROM unnest(ARRAY['brown NOT quick', '"fox brown"', 'quick OR fox', 'quick brown', 'col* OR colr~1',
		'{dog dogs}^2 OR lazy', 'Cafe', 'quick', 'c*', 'kolor~1', 'spam OR cat']) q;
	PERFORM set_config('sift_ref.mutant', '', false);
	RETURN r;
END $$;
SELECT m, pg_temp.answers(m) <> pg_temp.answers('') AS detected
FROM unnest(ARRAY['not_ignored', 'phrase_as_and', 'float8_score', 'idf_df_plus1', 'tie_ctid_desc', 'boost_ignored',
	'dismax_sum', 'topk_short', 'count_plus', 'nofold_query', 'tf_cap15', 'prefix_trunc', 'fuzzy_no_firstchar']) m;
DROP TABLE ref_docs;
RESET sift_ref.tokenizer;
\endif
