# Field weights and facets

What 3.1 adds for field weights and facets, how it works and what it leaves out, with the reasons. The comparison rows S6 and S7 in
[COMPARISON.md](COMPARISON.md) follow this page; the syntax is in [USAGE.md](USAGE.md).

- [Design](#design)
- [Which aggregate shapes run in SiftAgg](#which-aggregate-shapes-run-in-siftagg)
- [Not done, and why](#not-done-and-why)
- [Format and compatibility](#format-and-compatibility)
- [Tests](#tests)

## Design

### Facets: `sift.facet_counts`

```sql
SELECT * FROM sift.facet_counts('items', 'title', 'wireless',
  '[{"expr":"brand","size":10,"missing":true},
    {"expr":"price","edges":[100,300]},
    {"expr":"added","unit":"month","fill":true},
    {"expr":"tags","array":true}]',
  'active', hits => true);
-- (table, key expression, SiftQL query, facets, SQL filter, hits): facet, kind, value, lo, hi, count
```

`facets` is a JSON array; an element is a column expression (a terms facet) or an object:

| Key | Meaning |
|---|---|
| `name` | label of the facet in the output (default: `expr`) |
| `expr` | column expression of any type |
| `type` | `terms` (default; `range` with `edges`, `histogram` with `interval`, `date_histogram` with `unit`) |
| `size`, `order` | top N of a terms facet; `count` (default), `count asc`, `value`, `value desc` |
| `missing` | adds the bucket of the NULL values |
| `array` | the expression is an array: each element counts once per row |
| `edges` | range bounds: buckets `,e1) [e1,e2) ... [en,)` |
| `interval`, `offset` | histogram width and origin |
| `unit` | `second`, `minute`, `hour`, `day`, `week`, `month`, `quarter`, `year` (`date_trunc` on the `timestamp`) |
| `fill` | returns the empty buckets between the first and the last; more than 10000 buckets is an error |
| `selected` | the values the user chose (bucket numbers of a range, bucket starts of a histogram) |
| `filter` | a SQL condition the user chose in this facet |

Output: one row per bucket, `kind` is `value`, `bucket`, `other` (the values past `size`: `lo` is how many, `count` the sum of their counts),
`missing`, or `hits` (facet NULL; the rows left after every selection).

Rules:

- **Exact.** Every count is the `count(*)` of the same snapshot; there is no estimate. The other-values row is the exact total of the rest.
- **Multi-select.** The scan of a facet has the search, the shared `filter` and the selections of the other facets, not its own.
- **One statement.** `sift.facet_sql(...)` returns the SQL text; `facet_counts` runs it as the caller (permissions and row-level security apply).
- **Scans.** Each key facet is a `MATERIALIZED` `GROUP BY` scan of its own. All the range facets with the same `WHERE` share one scan of
  `count(*) FILTER (WHERE ...)`. An array facet unnests in a lateral `SELECT DISTINCT`, so a repeated element counts once per row.
- **Plan.** With a `sift_col` index on the columns, each scan is a `Custom Scan (SiftAgg)` with the search as the row selection and shows in
  `EXPLAIN` of `sift.facet_sql(...)`. Without one it is the ordinary plan, with the same result.

Why a scan per facet and not one `GROUPING SETS`: the first version used `GROUPING SETS`. With four expression keys the planner chose a
`SiftScan` and a `HashAgg` (54-163 ms on a million rows in the development of the feature, a 3.1 development build), and a single set is not pushed down. A scan per facet with
`FILTER` aggregates for the ranges was 7-13 times faster on the same build. The figures of the release (pg_sift 3.1.0, 1,000,000 rows, four facets: category, top 10
countries, two price ranges, month; `sift.facet_counts` against the same four `GROUP BY`s on the aggregate pushdown and on the normal plan, with date, host and load) are the generated tables

The planner's choice between the aggregate pushdown and a `SiftScan` with a normal aggregate flips with a cold or warm term-statistics cache
(`sift_estimate_cached`); the results are the same.

### Fields: `sift.sections` and `sift.field_query`

```sql
CREATE INDEX ON items USING sift (sift.sections(title, body, tags)) WITH (fields = 'title,body,tags');
SELECT id FROM items
WHERE sift.sections(title, body, tags) @@> sift.field_query('wireless headphones', 'title^3, body, tags^0.5')
ORDER BY sift.score(ctid) DESC LIMIT 10;
```

- `sift.sections(VARIADIC text[])` joins the sections with `chr(29)` (a NULL section is empty); the same expression is the key of the index and the
  left side of `@@>`.
- `sift.field_query(query, fields [, index])` rewrites a SiftQL query: every word, prefix, wildcard and fuzzy term outside a `name:` qualifier is
  searched in each listed field, with its boost times the weight of the field (`^w`, above 0 and up to 10000; no weight is 1). A row scores the
  sum of its fields' scores; the match set is that of the unweighted query on those fields. With `index`, the field names are checked against
  the index (`42601` for an unknown field, `22023` for an index with no `fields`).
- Phrases, regular expressions and the terms of a `NOT` are not scoped to a field (the section terms have no positions); they match the whole key.
- Top-k with weights returns the full ranking's top k (tested for 60 combinations of weights from 0.01 to 10000), and the scores equal a SQL
  per-field BM25 reference.

Combination rule: the sum of per-field BM25 scores, each field's term with its own `idf` and `tf` and the document's length normalisation.
This is Elasticsearch `most_fields` with boosts, not `combined_fields`.

## Which aggregate shapes run in SiftAgg

Probed with the search as the row selection (a `sift_col` index on the grouped columns):

| Shape | Result |
|---|---|
| terms on integer, text and boolean columns | `SiftAgg` |
| `date_trunc(...)` and `width_bucket(...)` keys, `CASE` keys | `SiftAgg` |
| `count(*) FILTER (WHERE ...)`, `min`, `max`, `IN` lists | `SiftAgg` |
| `GROUPING SETS` of up to 4 keys, and the empty set | `SiftAgg`, but the planner often prefers `SiftScan` plus `HashAgg` (see above) |
| `unnest` of an array column, `jsonb ->> 'k'` without `paths` | ordinary plan |
| an `OR` between the search and another predicate | ordinary plan |
| one grouping set with `GROUPING()` | ordinary plan |

`sift.facet_counts` is built from the first group only, so its scans run in `SiftAgg` when the columns are in the index. An array facet, or a
filter on a column outside the index, makes that one scan an ordinary plan; the counts stay exact.

## Not done, and why

| Not done | What it needs |
|---|---|
| A multicolumn BM25 index (`CREATE INDEX ... USING sift (title, body)`) | Key matching assumes one key (`indnatts = 1`) in the planner hook, the estimator, the count scan, the aggregate extraction, `sift.facets`, and the SQLite and MariaDB hosts. Each would learn a list of keys and `@@>` which key it means. `sift.sections` gives the same result through one key and one planner path, so the change was declined for 3.1. |
| BM25F (one combined score with per-field term frequency and length) | A document length per field: the docmap rows, the block-max metadata and the scoring kernels change, so the segment format changes. It needs the compatibility work (side-by-side read, upgrade tests, merge and `VACUUM` rewrite, a `REINDEX` hint) and a format id coordinated with the other 3.1 branches. Not attempted here. |
| An analyzer per field | Part of the same change: the section terms are made by the index's one analyzer. |
| Phrase and regular expression inside a field | The section terms have no positions. A recheck that knows the section borders would be needed, and a position format change for the index scan. |
| Highlight per field | `sift.highlight` works on the text of one key; it would need the border of each section. |
| Array facets and filters off the column index in `SiftAgg` | `SiftAgg` reads one value per row from `sift_col`; an array element per row is a different kernel. They run, exact, as the ordinary plan. |
| `sift.facets` on other than keyword text keys | Left as it is: it reads bitmaps of whole values. `sift.facet_counts` covers the other types. |

## Format and compatibility

No on-disk change: the BM25 segment formats, the metapage minor and the column format minor are those of the rest of 3.1 ([FORMATS.md](FORMATS.md)).
The extension SQL moves from 3.0 to 3.1: `ALTER EXTENSION pg_sift UPDATE` runs `pg_sift--3.0--3.1.sql`, which adds `sift.facet_cap`,
`sift.facet_part`, `sift.facet_sql`, `sift.facet_counts`, `sift.sections` and `sift.field_query` (and the other 3.1 objects), and the C function
`sift_field_query` of the library. No index needs a rebuild.

## Tests

- `facet_counts` (regress): 24 cases against hand-written oracles in every state (build, chunks, insert, update, delete, own transaction,
  rollback, `VACUUM`, promote, merge, parallel), with `SiftAgg` forced and with `sift.enable_agg = off`; a table with no column index; a
  `sift_table`; a role with limited permissions; the errors.
- `facet_counts_writes` (isolation): the counts against the `GROUP BY` while another session writes.
- `h_search_facet_scans`, `h_search_facet_counts` (column differential fixtures).
- `field_weights` (regress): weighted scores against the per-field SQL reference, top-k against the full ranking for 60 weight combinations,
  match sets against the unweighted query on the index and on a sequential scan.
- `extension_lifecycle` (TAP): the 3.0 to 3.1 update gives the same objects as a fresh 3.1 install, and `sift.facet_counts` answers after it.
