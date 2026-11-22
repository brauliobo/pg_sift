from .config import MEGA_ENTITY_ID


def setup_base_schema(pg):
    pg.exec_sql("""
        CREATE EXTENSION IF NOT EXISTS pg_sift;

        DROP TABLE IF EXISTS events CASCADE;
        DROP TABLE IF EXISTS entity_parents CASCADE;

        CREATE TABLE entity_parents (
            entity_id int NOT NULL,
            parent_id int NOT NULL,
            PRIMARY KEY (entity_id, parent_id)
        ) WITH (autovacuum_enabled = false);

        -- 2,000 entities mapped to 100 parents with realistic Zipf power-law skew:
        --   - Parents 1..94 follow Zipf weight p^(-0.85) (parent 1 ~13.4%, top 10 ~45.7% of entities)
        --   - Parents 95..100 are tiny 1-entity parents (6 single-entity parents)
        --   - ~97% of entities have 1 parent; ~3% have 2-6 parents (avg multiplicity ~1.05x)
        --     spread across parents 1..80 for realistic parent overlap.
        WITH parent_weights AS (
            SELECT p, (1.0 / power(p::float8, 0.85)) AS w
            FROM generate_series(1, 94) p
        ),
        parent_cdf AS (
            SELECT
                p,
                CASE WHEN p = 94 THEN 1.01
                     ELSE sum(w) OVER (ORDER BY p) / sum(w) OVER ()
                END AS hi,
                coalesce(sum(w) OVER (ORDER BY p ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), 0.0)
                    / sum(w) OVER () AS lo
            FROM parent_weights
        ),
        entity_primary AS (
            SELECT 1 AS entity_id, 1 AS parent_id
            UNION ALL
            SELECT e.entity_id, pc.p AS parent_id
            FROM (
                SELECT
                    g AS entity_id,
                    ((((g * 7919 + 104729) % 1000000) + 0.5) / 1000000.0) AS u
                FROM generate_series(2, 1994) g
            ) e
            JOIN parent_cdf pc ON e.u >= pc.lo AND e.u < pc.hi
            UNION ALL
            SELECT 1994 + k AS entity_id, 94 + k AS parent_id
            FROM generate_series(1, 6) k
        ),
        entity_extra AS (
            SELECT
                e,
                1 + ((e * 37 + j * 19) % 80) AS parent_id
            FROM generate_series(2, 1994) e,
                 generate_series(1, 5) j
            WHERE (e % 50 = 0 AND j = 1)
               OR (e % 166 = 17 AND j <= 2)
               OR (e IN (11, 211, 411) AND j <= 3)
               OR (e IN (611, 811, 1011) AND j <= 4)
               OR (e IN (1211, 1411) AND j <= 5)
        )
        INSERT INTO entity_parents (entity_id, parent_id)
        SELECT entity_id, parent_id FROM entity_primary
        UNION
        SELECT e, parent_id FROM entity_extra
        ON CONFLICT DO NOTHING;

        CREATE INDEX entity_parents_journal
            ON entity_parents USING sift_col (entity_id, parent_id)
            WITH (track_changes = 'ctid');

        VACUUM (ANALYZE, FREEZE) entity_parents;
    """)
    row = pg.query_rows("""
        WITH ec AS (
            SELECT entity_id, count(*) AS np FROM entity_parents GROUP BY entity_id
        ),
        pc AS (
            SELECT parent_id, count(*) AS ne FROM entity_parents GROUP BY parent_id
        ),
        ov AS (
            SELECT count(DISTINCT ep1.parent_id) AS n_ov
            FROM entity_parents ep1
            JOIN entity_parents ep2
              ON ep2.entity_id = ep1.entity_id AND ep2.parent_id <> ep1.parent_id
        )
        SELECT
            round((SELECT count(*)::numeric / count(DISTINCT entity_id) FROM entity_parents), 3),
            (SELECT ne FROM pc WHERE parent_id = 1),
            round(100.0 * (SELECT sum(ne) FROM pc WHERE parent_id <= 10) / (SELECT sum(ne) FROM pc), 1),
            round(100.0 * (SELECT count(*) FROM ec WHERE np = 1) / (SELECT count(*) FROM ec), 1),
            (SELECT max(np) FROM ec),
            round(100.0 * (SELECT n_ov FROM ov) / (SELECT count(*) FROM pc), 1)
    """)[0]
    return {
        "mult": float(row[0]),
        "p1_entities": int(row[1]),
        "top10_pct": float(row[2]),
        "single_parent_pct": float(row[3]),
        "max_mult": int(row[4]),
        "overlap_parents_pct": float(row[5]),
    }


def events_table_ddl(table_name):
    return f"""
        DROP TABLE IF EXISTS {table_name} CASCADE;
        CREATE TABLE {table_name} (
            id           bigint PRIMARY KEY,
            entity_id    int NOT NULL,
            event_date   date NOT NULL,
            deleted_at   timestamptz,
            updated_at   timestamptz NOT NULL,
            group_key    int NOT NULL,
            distinct_tag text NOT NULL,
            status       smallint NOT NULL,
            amount       numeric(12,2) NOT NULL,
            score        float8 NOT NULL,
            qty          int NOT NULL,
            event_uuid   uuid NOT NULL,
            metrics      jsonb NOT NULL,
            notes        jsonb NOT NULL,
            analysis     jsonb NOT NULL
        ) WITH (autovacuum_enabled = false);
    """


def events_insert_sql(table_name, id_lo, id_hi, date_offset=0):
    # Zipf entity skew: 5% rows on mega-entity 1, 95% power-law across entities 2..2000.
    # Soft-deletes: ~0.1% global (83% on historical event_date < 2025-01-01).
    # Sparse analysis JSONB: 25% populated with 'flag' + 2 items, 75% empty '{}'.
    return f"""
        INSERT INTO {table_name}
        SELECT
            i,
            CASE WHEN (i % 20) = 0 THEN {MEGA_ENTITY_ID}
                 ELSE LEAST(2000, 2 + floor(1999.0 * power((((i * 7919 + 104729) % 1000000) + 0.5) / 1000000.0, 2.2))::int)
            END,
            DATE '2024-01-01' + (((i + {date_offset}) % 730)::int),
            CASE WHEN (((i + {date_offset}) % 730) < 365 AND ((i * 17) % 600) = 7)
                   OR (((i + {date_offset}) % 730) >= 365 AND ((i * 17) % 3000) = 7)
                 THEN TIMESTAMPTZ '2025-11-01 00:00:00+00' + ((i % 45) * INTERVAL '1 day')
                 ELSE NULL
            END,
            TIMESTAMPTZ '2025-01-01 00:00:00+00' + (i * INTERVAL '180 seconds') + (((i * 97) % 1800) * INTERVAL '1 second'),
            1 + ((i * 131) % 50000),
            'tag-' || ((i * 97) % 50000),
            (i % 100)::smallint,
            (((i * 104729) % 100000) / 100.0)::numeric(12,2),
            (((i * 31) % 1000) / 10.0)::float8,
            1 + (i % 50),
            md5(i::text)::uuid,
            jsonb_build_object(
                'channel', (ARRAY['web','app','api','partner'])[1 + (i % 4)],
                'latency', ((i % 500) / 10.0),
                'region',  (ARRAY['na','eu','sa','ap'])[1 + ((i / 4) % 4)]
            ),
            jsonb_build_object(
                'tag',      'n-' || (i % 500),
                'priority', (i % 5),
                'summary',  repeat('x', 64) || '-' || (i % 1000)
            ),
            CASE WHEN ((i + (i / 20)) % 4) = 0 THEN
                jsonb_build_object(
                    'flag', 'ok',
                    'items', jsonb_build_array(
                        jsonb_build_object('kind', 'k' || (i % 12), 'weight', ((i % 100) / 10.0)),
                        jsonb_build_object('kind', 'k' || ((i + 5) % 12), 'weight', (((i * 3) % 100) / 10.0))
                    )
                )
            ELSE '{{}}'::jsonb
            END
        FROM generate_series({id_lo}::bigint, {id_hi}::bigint) i;
    """


def load_events_table(pg, nrows):
    pg.exec_sql(
        events_table_ddl("events")
        + events_insert_sql("events", 1, nrows)
        + "\nVACUUM (ANALYZE, FREEZE) events;\n"
    )
